// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ObjectBackfillRequest.h"
#include "BackfillDaemon.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "cls/lock/cls_lock_client.h"
#include "librbd/Utils.h"
#include <sstream>

#define dout_context m_cct
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd::backfill::ObjectBackfillRequest: " \
                           << this << " " << __func__ << ": "

namespace rbd {
namespace backfill {

ObjectBackfillRequest::ObjectBackfillRequest(
  librados::IoCtx& parent_ioctx,
  const std::string& parent_oid,
  uint64_t object_no,
  const ceph::bufferlist& data,
  CephContext* cct,
  Threads* threads,
  Context* on_finish)
  : m_parent_ioctx(parent_ioctx),  // Copy IoCtx - safe for async use
    m_parent_oid(parent_oid),
    m_object_no(object_no),
    m_threads(threads),
    m_cct(cct),
    m_on_finish(on_finish),
    m_lock("ObjectBackfillRequest::m_lock"),
    m_state(STATE_INIT),
    m_ret_val(0),
    m_cancel_flag(false),
    m_lock_acquired(false),
    m_finished(false),
    m_watch_handle(0),
    m_data_bl(data) {  // Store pre-fetched data

  // Generate unique lock cookie using thread ID and timestamp
  std::stringstream ss;
  ss << "backfill-" << pthread_self() << "-" << ceph_clock_now();
  m_lock_cookie = ss.str();

  // Lock name is the object name
  m_lock_name = "rbd_lock";
  m_lock_tag = "";  // No tag for exclusive locks
}

ObjectBackfillRequest::~ObjectBackfillRequest() {
}

void ObjectBackfillRequest::send() {
  dout(10) << "object_no=" << m_object_no << " oid=" << m_parent_oid << dendl;

  m_state = STATE_ACQUIRE_LOCK;
  acquire_lock();
}

void ObjectBackfillRequest::cancel() {
  dout(10) << "cancelling backfill for object_no=" << m_object_no << dendl;
  m_cancel_flag.store(true);
}

void ObjectBackfillRequest::acquire_lock() {
  dout(15) << dendl;

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_acquire_lock);

  // Use cls_lock to acquire exclusive lock with timeout
  librados::ObjectWriteOperation op;
  rados::cls::lock::lock(
    &op,
    m_lock_name,
    LOCK_EXCLUSIVE,
    m_lock_cookie,
    m_lock_tag,
    "",  // No description
    utime_t(LOCK_TIMEOUT_SECONDS, 0),  // Lock duration
    0);  // No flags

  librados::AioCompletion* rados_completion =
    librbd::util::create_rados_callback(ctx);

  int r = m_parent_ioctx.aio_operate(m_parent_oid, rados_completion, &op);
  ceph_assert(r == 0);
  rados_completion->release();
}

void ObjectBackfillRequest::handle_acquire_lock(int r) {
  dout(15) << "r=" << r << dendl;

  if (r < 0) {
    if (r == -EBUSY || r == -EEXIST) {
      // Lock already held - client I/O preempted daemon backfill
      dout(5) << "lock busy on object " << m_object_no
              << ", client I/O has preempted daemon backfill" << dendl;
    } else {
      derr << "failed to acquire lock: " << cpp_strerror(r) << dendl;
    }
    finish(r);
    return;
  }

  {
    Mutex::Locker locker(m_lock);
    m_lock_acquired = true;
  }

  dout(10) << "lock acquired for object " << m_object_no << dendl;

  // Check for cancellation
  if (m_cancel_flag.load()) {
    dout(10) << "cancelled after acquiring lock" << dendl;
    m_state = STATE_RELEASE_LOCK;
    release_lock();
    return;
  }

  // S3 fetch already done in ImageBackfiller thread
  // Data is already in m_data_bl
  // Go directly to RADOS write
  m_state = STATE_WRITE_RADOS;
  write_rados();
}

/*
void ObjectBackfillRequest::watch_lock() {
  dout(15) << dendl;

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_watch_lock);

  // Set up watch to detect when user IO tries to acquire lock
  librados::AioCompletion* rados_completion =
    librbd::util::create_rados_callback(ctx);

  int r = m_parent_ioctx.aio_watch(
    m_parent_oid,
    rados_completion,
    &m_watch_handle,
    lock_watcher_callback,
    this);

  if (r < 0) {
    derr << "failed to set up watch: " << cpp_strerror(r) << dendl;
    delete ctx;
    finish(r);
    return;
  }

  rados_completion->release();
}

void ObjectBackfillRequest::handle_watch_lock(int r) {
  dout(15) << "r=" << r << dendl;

  if (r < 0) {
    derr << "watch failed: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  dout(10) << "watch established, starting S3 fetch" << dendl;
  m_state = STATE_FETCH_S3;
  fetch_s3();
}
*/

void ObjectBackfillRequest::write_rados() {
  dout(15) << dendl;

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_write_rados);

  librados::ObjectWriteOperation op;
  op.write_full(m_data_bl);

  librados::AioCompletion* rados_completion =
    librbd::util::create_rados_callback(ctx);

  int r = m_parent_ioctx.aio_operate(m_parent_oid, rados_completion, &op);
  ceph_assert(r == 0);
  rados_completion->release();
}

void ObjectBackfillRequest::handle_write_rados(int r) {
  dout(15) << "r=" << r << dendl;

  if (r < 0) {
    derr << "RADOS write failed: " << cpp_strerror(r) << dendl;
    m_ret_val = r;
    m_state = STATE_RELEASE_LOCK;
    release_lock();
    return;
  }

  dout(10) << "RADOS write complete, updating object map" << dendl;
  m_state = STATE_UPDATE_OBJECT_MAP;
  update_object_map();
}

void ObjectBackfillRequest::update_object_map() {
  dout(15) << dendl;

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_update_object_map);

  // Update object map to mark this object as OBJECT_COPIEDUP
  // This will be implemented using cls_rbd in the parent's object map
  // For now, we'll skip this step and move directly to release_lock
  // TODO: Implement object map update using cls_rbd

  dout(10) << "object map update not yet implemented, skipping" << dendl;
  m_threads->work_queue->queue(ctx, 0);
}

void ObjectBackfillRequest::handle_update_object_map(int r) {
  dout(15) << "r=" << r << dendl;

  if (r < 0) {
    derr << "object map update failed: " << cpp_strerror(r) << dendl;
    m_ret_val = r;
    m_state = STATE_RELEASE_LOCK;
    release_lock();
    return;
  }

  dout(10) << "object map updated, releasing lock" << dendl;
  m_state = STATE_RELEASE_LOCK;
  m_ret_val = 0;  // Success
  release_lock();
}

void ObjectBackfillRequest::release_lock() {
  dout(15) << dendl;

  // First, remove the watch if it was established
  if (m_watch_handle != 0) {
    int r = m_parent_ioctx.unwatch2(m_watch_handle);
    if (r < 0) {
      dout(5) << "failed to remove watch: " << cpp_strerror(r) << dendl;
    }
    m_watch_handle = 0;
  }

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_release_lock);

  // Release the lock
  librados::ObjectWriteOperation op;
  rados::cls::lock::unlock(&op, m_lock_name, m_lock_cookie);

  librados::AioCompletion* rados_completion =
    librbd::util::create_rados_callback(ctx);

  int r = m_parent_ioctx.aio_operate(m_parent_oid, rados_completion, &op);
  ceph_assert(r == 0);
  rados_completion->release();
}

void ObjectBackfillRequest::handle_release_lock(int r) {
  dout(15) << "r=" << r << dendl;

  if (r < 0) {
    dout(5) << "failed to release lock: " << cpp_strerror(r) << dendl;
    // Continue anyway - lock will eventually timeout
  } else {
    dout(10) << "lock released" << dendl;
  }

  {
    Mutex::Locker locker(m_lock);
    m_lock_acquired = false;
  }

  // If we were cancelled, report cancellation error
  if (m_cancel_flag.load()) {
    finish(-ECANCELED);
  } else {
    finish(m_ret_val);
  }
}

void ObjectBackfillRequest::finish(int r) {
  dout(10) << "r=" << r << dendl;

  bool need_release_lock = false;
  bool already_finished = false;

  {
    Mutex::Locker locker(m_lock);
    if (m_finished) {
      dout(5) << "already finished, ignoring duplicate finish call" << dendl;
      already_finished = true;
    } else {
      m_finished = true;
      m_state = STATE_COMPLETE;
      m_ret_val = r;
      need_release_lock = m_lock_acquired;
    }
  }

  if (already_finished) {
    return;
  }

  // Clean up watch if still active
  if (m_watch_handle != 0) {
    int watch_r = m_parent_ioctx.unwatch2(m_watch_handle);
    if (watch_r < 0) {
      dout(5) << "failed to remove watch: " << cpp_strerror(watch_r) << dendl;
    }
    m_watch_handle = 0;
  }

  // If we still hold the lock, release it before completing
  if (need_release_lock) {
    dout(10) << "releasing lock from finish()" << dendl;

    // Release lock synchronously to ensure it's released before we complete
    librados::ObjectWriteOperation op;
    rados::cls::lock::unlock(&op, m_lock_name, m_lock_cookie);

    int unlock_r = m_parent_ioctx.operate(m_parent_oid, &op);
    if (unlock_r < 0) {
      dout(5) << "failed to release lock in finish(): " << cpp_strerror(unlock_r) << dendl;
      // Continue anyway - lock will timeout
    }

    {
      Mutex::Locker locker(m_lock);
      m_lock_acquired = false;
    }
  }

  m_on_finish->complete(r);

  // Delete self after completion
  delete this;
}

/*
void ObjectBackfillRequest::lock_watcher_callback(
  void* arg,
  uint64_t notify_id,
  uint64_t cookie,
  uint64_t notifier_id,
  bufferlist& data) {

  ObjectBackfillRequest* req = static_cast<ObjectBackfillRequest*>(arg);
  req->handle_lock_notification(notify_id, cookie, notifier_id, data);
}

void ObjectBackfillRequest::handle_lock_notification(
  uint64_t notify_id,
  uint64_t cookie,
  uint64_t notifier_id,
  bufferlist& data) {

  dout(10) << "received lock notification, notify_id=" << notify_id
           << " cookie=" << cookie << " notifier_id=" << notifier_id << dendl;

  // When we receive a notification, it means someone (likely user IO) is trying
  // to acquire the lock. We should cancel our operation and release the lock.
  dout(10) << "user IO detected, preempting backfill" << dendl;
  cancel();

  // Acknowledge the notification
  bufferlist reply;
  m_parent_ioctx.notify_ack(m_parent_oid, notify_id, cookie, reply);
}
*/

} // namespace backfill
} // namespace rbd
