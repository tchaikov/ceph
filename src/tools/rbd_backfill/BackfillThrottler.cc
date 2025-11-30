// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "BackfillThrottler.h"
#include "include/Context.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"

#define dout_context m_cct
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd::backfill::BackfillThrottler: " << this << " " << __func__ << ": "

namespace rbd {
namespace backfill {

BackfillThrottler::BackfillThrottler(CephContext *cct, ContextWQ *work_queue)
  : m_cct(cct),
    m_work_queue(work_queue),
    m_lock("rbd::backfill::BackfillThrottler::m_lock") {
  m_max_concurrent = m_cct->_conf.get_val<uint64_t>("rbd_backfill_max_concurrent");
  dout(10) << "max_concurrent=" << m_max_concurrent << dendl;
}

BackfillThrottler::~BackfillThrottler() {
  dout(10) << dendl;
  Mutex::Locker locker(m_lock);

  ceph_assert(m_inflight_ops.empty());
  ceph_assert(m_queued_ops.empty());
}

void BackfillThrottler::start_op(uint64_t obj_no, Context *on_start) {
  dout(20) << "obj_no=" << obj_no << dendl;

  Mutex::Locker locker(m_lock);

  if (m_inflight_ops.size() < m_max_concurrent) {
    // Can start immediately
    dout(15) << "starting obj_no=" << obj_no << " immediately (inflight="
             << m_inflight_ops.size() << "/" << m_max_concurrent << ")" << dendl;
    m_inflight_ops.insert(obj_no);
    m_work_queue->queue(on_start, 0);
  } else {
    // Queue for later
    dout(15) << "queuing obj_no=" << obj_no << " (inflight="
             << m_inflight_ops.size() << "/" << m_max_concurrent
             << ", queued=" << m_queued_ops.size() << ")" << dendl;
    m_queued_ops.push_back({obj_no, on_start});
  }
}

void BackfillThrottler::finish_op(uint64_t obj_no) {
  dout(20) << "obj_no=" << obj_no << dendl;

  Mutex::Locker locker(m_lock);

  auto it = m_inflight_ops.find(obj_no);
  if (it == m_inflight_ops.end()) {
    derr << "obj_no=" << obj_no << " not in inflight set!" << dendl;
    return;
  }

  m_inflight_ops.erase(it);
  dout(15) << "finished obj_no=" << obj_no << " (inflight="
           << m_inflight_ops.size() << "/" << m_max_concurrent << ")" << dendl;

  // Signal if all operations are complete - use SignalAll() to wake all waiters
  if (m_inflight_ops.empty()) {
    m_cond.SignalAll();
  }

  // Start next queued operation if available
  start_next_op();
}

void BackfillThrottler::start_next_op() {
  ceph_assert(m_lock.is_locked());

  if (m_queued_ops.empty()) {
    dout(20) << "no queued ops" << dendl;
    return;
  }

  if (m_inflight_ops.size() >= m_max_concurrent) {
    dout(20) << "at max concurrency" << dendl;
    return;
  }

  auto queued = m_queued_ops.front();
  m_queued_ops.pop_front();

  dout(15) << "starting queued obj_no=" << queued.obj_no << " (inflight="
           << m_inflight_ops.size() << "/" << m_max_concurrent
           << ", queued=" << m_queued_ops.size() << ")" << dendl;

  m_inflight_ops.insert(queued.obj_no);
  m_work_queue->queue(queued.on_start, 0);
}

void BackfillThrottler::set_max_concurrent(uint32_t max_concurrent) {
  dout(10) << "max_concurrent=" << max_concurrent << dendl;

  Mutex::Locker locker(m_lock);
  m_max_concurrent = max_concurrent;

  // Start queued ops if we have capacity now
  while (m_inflight_ops.size() < m_max_concurrent && !m_queued_ops.empty()) {
    start_next_op();
  }
}

uint32_t BackfillThrottler::get_max_concurrent() const {
  Mutex::Locker locker(m_lock);
  return m_max_concurrent;
}

void BackfillThrottler::get_status(uint32_t *inflight, uint32_t *queued) const {
  Mutex::Locker locker(m_lock);
  if (inflight != nullptr) {
    *inflight = m_inflight_ops.size();
  }
  if (queued != nullptr) {
    *queued = m_queued_ops.size();
  }
}

void BackfillThrottler::wait_for_ops() {
  dout(10) << dendl;

  Mutex::Locker locker(m_lock);
  while (!m_inflight_ops.empty()) {
    dout(15) << "waiting for " << m_inflight_ops.size()
             << " in-flight operations" << dendl;
    m_cond.Wait(m_lock);
  }

  dout(10) << "all operations complete" << dendl;
}

} // namespace backfill
} // namespace rbd
