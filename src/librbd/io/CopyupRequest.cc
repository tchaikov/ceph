// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/CopyupRequest.h"
#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Mutex.h"
#include "common/Timer.h"
#include "common/WorkQueue.h"
#include "librbd/AsyncObjectThrottle.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"
#include "librbd/io/AsyncWritebackThrottler.h"
#include "librbd/io/S3ObjectFetcher.h"
#include "librbd/deep_copy/ObjectCopyRequest.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/ImageRequest.h"
#include "librbd/io/ObjectRequest.h"
#include "librbd/io/ReadResult.h"
#include "cls/lock/cls_lock_client.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/construct.hpp>
#include <iomanip>
#include <sstream>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::CopyupRequest: " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace io {

namespace {

template <typename I>
class C_UpdateObjectMap : public C_AsyncObjectThrottle<I> {
public:
  C_UpdateObjectMap(AsyncObjectThrottle<I> &throttle, I *image_ctx,
                    uint64_t object_no, uint8_t head_object_map_state,
                    const std::vector<uint64_t> *snap_ids,
                    bool first_snap_is_clean, const ZTracer::Trace &trace,
                    size_t snap_id_idx)
    : C_AsyncObjectThrottle<I>(throttle, *image_ctx), m_object_no(object_no),
      m_head_object_map_state(head_object_map_state), m_snap_ids(*snap_ids),
      m_first_snap_is_clean(first_snap_is_clean), m_trace(trace),
      m_snap_id_idx(snap_id_idx)
  {
  }

  int send() override {
    auto& image_ctx = this->m_image_ctx;
    ceph_assert(image_ctx.owner_lock.is_locked());
    if (image_ctx.exclusive_lock == nullptr) {
      return 1;
    }
    ceph_assert(image_ctx.exclusive_lock->is_lock_owner());

    RWLock::RLocker snap_locker(image_ctx.snap_lock);
    if (image_ctx.object_map == nullptr) {
      return 1;
    }

    uint64_t snap_id = m_snap_ids[m_snap_id_idx];
    if (snap_id == CEPH_NOSNAP) {
      return update_head();
    } else {
      return update_snapshot(snap_id);
    }
  }

  int update_head() {
    auto& image_ctx = this->m_image_ctx;
    RWLock::WLocker object_map_locker(image_ctx.object_map_lock);
    bool sent = image_ctx.object_map->template aio_update<Context>(
      CEPH_NOSNAP, m_object_no, m_head_object_map_state, {}, m_trace, false,
      this);
    return (sent ? 0 : 1);
  }

  int update_snapshot(uint64_t snap_id) {
    auto& image_ctx = this->m_image_ctx;
    uint8_t state = OBJECT_EXISTS;
    if (image_ctx.test_features(RBD_FEATURE_FAST_DIFF, image_ctx.snap_lock) &&
        (m_snap_id_idx > 0 || m_first_snap_is_clean)) {
      // first snapshot should be exists+dirty since it contains
      // the copyup data -- later snapshots inherit the data.
      state = OBJECT_EXISTS_CLEAN;
    }

    RWLock::RLocker object_map_locker(image_ctx.object_map_lock);
    bool sent = image_ctx.object_map->template aio_update<Context>(
      snap_id, m_object_no, state, {}, m_trace, true, this);
    ceph_assert(sent);
    return 0;
  }

private:
  uint64_t m_object_no;
  uint8_t m_head_object_map_state;
  const std::vector<uint64_t> &m_snap_ids;
  bool m_first_snap_is_clean;
  const ZTracer::Trace &m_trace;
  size_t m_snap_id_idx;
};

} // anonymous namespace

template <typename I>
CopyupRequest<I>::CopyupRequest(I *ictx, const std::string &oid,
                                uint64_t objectno, Extents &&image_extents,
                                const ZTracer::Trace &parent_trace)
  : m_image_ctx(ictx), m_oid(oid), m_object_no(objectno),
    m_image_extents(image_extents),
    m_trace(util::create_trace(*m_image_ctx, "copy-up", parent_trace)),
    m_lock("CopyupRequest", false, false)
{
  ceph_assert(m_image_ctx->data_ctx.is_valid());
  m_async_op.start_op(*util::get_image_ctx(m_image_ctx));
}

template <typename I>
CopyupRequest<I>::~CopyupRequest() {
  ceph_assert(m_pending_requests.empty());
  m_async_op.finish_op();
}

template <typename I>
void CopyupRequest<I>::append_request(AbstractObjectWriteRequest<I> *req) {
  Mutex::Locker locker(m_lock);

  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "oid=" << m_oid << ", "
                 << "object_request=" << req << ", "
                 << "append=" << m_append_request_permitted << dendl;
  if (m_append_request_permitted) {
    m_pending_requests.push_back(req);
  } else {
    m_restart_requests.push_back(req);
  }
}

template <typename I>
void CopyupRequest<I>::send() {
  read_from_parent();
}

template <typename I>
void CopyupRequest<I>::read_from_parent(bool skip_s3_check) {
  auto cct = m_image_ctx->cct;

  RWLock::RLocker snap_locker(m_image_ctx->snap_lock);
  RWLock::RLocker parent_locker(m_image_ctx->parent_lock);

  if (m_image_ctx->parent == nullptr) {
    ldout(cct, 5) << "parent detached" << dendl;

    m_image_ctx->op_work_queue->queue(
      util::create_context_callback<
        CopyupRequest<I>, &CopyupRequest<I>::handle_read_from_parent>(this),
      -ENOENT);
    return;
  } else if (is_deep_copy()) {
    deep_copy();
    return;
  }

  // For standalone clones with S3 back-fill enabled, check if parent object
  // exists in RADOS before reading. If it doesn't exist, fetch from S3 directly.
  // This avoids the sparse-read conversion that would prevent S3 back-fill.
  // skip_s3_check==true means handle_check_parent_object_exists already ran
  // the stat and decided that a normal RADOS read is the right next step.
  //
  // NOTE: we extract parent_oid and parent_ioctx HERE while holding parent_lock,
  // then release the lock before the async stat.  check_parent_object_exists()
  // must NOT re-acquire parent_lock: RWLock is non-recursive and will deadlock if
  // a writer is waiting (PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP).
  if (!skip_s3_check && should_fetch_from_s3()) {
    ldout(cct, 15) << "S3 back-fill enabled, checking parent object existence" << dendl;
    std::string parent_oid = m_image_ctx->parent->get_object_name(m_object_no);
    librados::IoCtx parent_ioctx = m_image_ctx->parent->data_ctx;
    // parent_locker and snap_locker go out of scope here (lock released before async I/O)
    check_parent_object_exists(std::move(parent_oid), std::move(parent_ioctx));
    return;
  }

  auto comp = AioCompletion::create_and_start<
    CopyupRequest<I>,
    &CopyupRequest<I>::handle_read_from_parent>(
      this, util::get_image_ctx(m_image_ctx->parent), AIO_TYPE_READ);

  ldout(cct, 20) << "oid=" << m_oid << ", "
                 << "completion=" << comp << ", "
                 << "extents=" << m_image_extents
                 << dendl;
  ImageRequest<I>::aio_read(m_image_ctx->parent, comp,
                            std::move(m_image_extents),
                            ReadResult{&m_copyup_data}, 0, m_trace);
}

template <typename I>
void CopyupRequest<I>::handle_read_from_parent(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "oid=" << m_oid << ", r=" << r << dendl;

  m_image_ctx->snap_lock.get_read();
  m_lock.Lock();
  m_copyup_is_zero = m_copyup_data.is_zero();
  m_copyup_required = is_copyup_required();
  disable_append_requests();

  if (r < 0 && r != -ENOENT) {
    m_lock.Unlock();
    m_image_ctx->snap_lock.put_read();

    lderr(cct) << "error reading from parent: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  if (!m_copyup_required) {
    m_lock.Unlock();
    m_image_ctx->snap_lock.put_read();

    ldout(cct, 20) << "no-op, skipping" << dendl;
    finish(0);
    return;
  }

  // copyup() will affect snapshots only if parent data is not all
  // zeros.
  if (!m_copyup_is_zero) {
    m_snap_ids.insert(m_snap_ids.end(), m_image_ctx->snaps.rbegin(),
                      m_image_ctx->snaps.rend());
  }

  m_lock.Unlock();
  m_image_ctx->snap_lock.put_read();

  update_object_maps();
}

template <typename I>
void CopyupRequest<I>::deep_copy() {
  auto cct = m_image_ctx->cct;
  ceph_assert(m_image_ctx->snap_lock.is_locked());
  ceph_assert(m_image_ctx->parent_lock.is_locked());
  ceph_assert(m_image_ctx->parent != nullptr);

  m_lock.Lock();
  m_flatten = is_copyup_required() ? true : m_image_ctx->migration_info.flatten;
  m_lock.Unlock();

  ldout(cct, 20) << "oid=" << m_oid << ", flatten=" << m_flatten << dendl;

  auto ctx = util::create_context_callback<
    CopyupRequest<I>, &CopyupRequest<I>::handle_deep_copy>(this);
  auto req = deep_copy::ObjectCopyRequest<I>::create(
    m_image_ctx->parent, m_image_ctx, m_image_ctx->migration_info.snap_map,
    m_object_no, m_flatten, ctx);

  req->send();
}

template <typename I>
void CopyupRequest<I>::handle_deep_copy(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "oid=" << m_oid << ", r=" << r << dendl;

  m_image_ctx->snap_lock.get_read();
  m_lock.Lock();
  m_copyup_required = is_copyup_required();
  if (r == -ENOENT && !m_flatten && m_copyup_required) {
    m_lock.Unlock();
    m_image_ctx->snap_lock.put_read();

    ldout(cct, 10) << "restart deep-copy with flatten" << dendl;
    send();
    return;
  }

  disable_append_requests();

  if (r < 0 && r != -ENOENT) {
    m_lock.Unlock();
    m_image_ctx->snap_lock.put_read();

    lderr(cct) << "error encountered during deep-copy: " << cpp_strerror(r)
               << dendl;
    finish(r);
    return;
  }

  if (!m_copyup_required && !is_update_object_map_required(r)) {
    m_lock.Unlock();
    m_image_ctx->snap_lock.put_read();

    if (r == -ENOENT) {
      r = 0;
    }

    ldout(cct, 20) << "skipping" << dendl;
    finish(r);
    return;
  }

  // For deep-copy, copyup() will never affect snapshots.  However,
  // this state machine is responsible for updating object maps for
  // snapshots that have been created on destination image after
  // migration started.
  if (r != -ENOENT) {
    compute_deep_copy_snap_ids();
  }

  m_lock.Unlock();
  m_image_ctx->snap_lock.put_read();

  update_object_maps();
}

template <typename I>
void CopyupRequest<I>::update_object_maps() {
  RWLock::RLocker owner_locker(m_image_ctx->owner_lock);
  RWLock::RLocker snap_locker(m_image_ctx->snap_lock);
  if (m_image_ctx->object_map == nullptr) {
    snap_locker.unlock();
    owner_locker.unlock();

    copyup();
    return;
  }

  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "oid=" << m_oid << dendl;

  bool copy_on_read = m_pending_requests.empty();
  uint8_t head_object_map_state = OBJECT_EXISTS;

  bool is_standalone_parent;
  {
    RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
    is_standalone_parent =
      (m_image_ctx->parent_md.parent_type == PARENT_TYPE_STANDALONE ||
       m_image_ctx->parent_md.parent_type == PARENT_TYPE_REMOTE_STANDALONE);
  }

  // FAST_DIFF can mark HEAD as OBJECT_EXISTS_CLEAN (data tied to first snap),
  // but only for snapshot-based clones — S3-fetched data does not match any
  // snapshot chain, so leave it at OBJECT_EXISTS for standalone parents.
  if (!is_standalone_parent && copy_on_read && !m_snap_ids.empty() &&
      m_image_ctx->test_features(RBD_FEATURE_FAST_DIFF,
                                 m_image_ctx->snap_lock)) {
    head_object_map_state = OBJECT_EXISTS_CLEAN;
  }

  auto r_it = m_pending_requests.rbegin();
  if (r_it != m_pending_requests.rend()) {
    // last write-op determines the final object map state
    head_object_map_state = (*r_it)->get_pre_write_object_map_state();
  }

  RWLock::WLocker object_map_locker(m_image_ctx->object_map_lock);
  if ((*m_image_ctx->object_map)[m_object_no] != head_object_map_state) {
    // (maybe) need to update the HEAD object map state
    m_snap_ids.push_back(CEPH_NOSNAP);
  }
  object_map_locker.unlock();
  snap_locker.unlock();

  ceph_assert(m_image_ctx->exclusive_lock->is_lock_owner());
  typename AsyncObjectThrottle<I>::ContextFactory context_factory(
    boost::lambda::bind(boost::lambda::new_ptr<C_UpdateObjectMap<I>>(),
    boost::lambda::_1, m_image_ctx, m_object_no, head_object_map_state,
    &m_snap_ids, m_first_snap_is_clean, m_trace, boost::lambda::_2));
  auto ctx = util::create_context_callback<
    CopyupRequest<I>, &CopyupRequest<I>::handle_update_object_maps>(this);
  auto throttle = new AsyncObjectThrottle<I>(
    nullptr, *m_image_ctx, context_factory, ctx, nullptr, 0, m_snap_ids.size());
  throttle->start_ops(
    m_image_ctx->config.template get_val<uint64_t>("rbd_concurrent_management_ops"));
}

template <typename I>
void CopyupRequest<I>::handle_update_object_maps(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "oid=" << m_oid << ", r=" << r << dendl;

  if (r < 0) {
    lderr(m_image_ctx->cct) << "failed to update object map: "
                            << cpp_strerror(r) << dendl;

    finish(r);
    return;
  }

  copyup();
}

template <typename I>
void CopyupRequest<I>::copyup() {
  auto cct = m_image_ctx->cct;
  m_image_ctx->snap_lock.get_read();
  auto snapc = m_image_ctx->snapc;
  m_image_ctx->snap_lock.put_read();

  m_lock.Lock();
  if (!m_copyup_required) {
    m_lock.Unlock();

    ldout(cct, 20) << "skipping copyup" << dendl;
    finish(0);
    return;
  }

  ldout(cct, 20) << "oid=" << m_oid << dendl;

  bool copy_on_read = m_pending_requests.empty();
  bool deep_copyup = !snapc.snaps.empty() && !m_copyup_is_zero;
  if (m_copyup_is_zero) {
    m_copyup_data.clear();
  }

  int r;
  librados::ObjectWriteOperation copyup_op;
  if (copy_on_read || deep_copyup) {
    copyup_op.exec("rbd", "copyup", m_copyup_data);
    ObjectRequest<I>::add_write_hint(*m_image_ctx, &copyup_op);
    ++m_pending_copyups;
  }

  librados::ObjectWriteOperation write_op;
  if (!copy_on_read) {
    if (!deep_copyup) {
      write_op.exec("rbd", "copyup", m_copyup_data);
      ObjectRequest<I>::add_write_hint(*m_image_ctx, &write_op);
    }

    // merge all pending write ops into this single RADOS op
    for (auto req : m_pending_requests) {
      ldout(cct, 20) << "add_copyup_ops " << req << dendl;
      req->add_copyup_ops(&write_op);
    }

    if (write_op.size() > 0) {
      ++m_pending_copyups;
    }
  }
  m_lock.Unlock();

  // issue librados ops at the end to simplify test cases
  std::vector<librados::snap_t> snaps;
  if (copyup_op.size() > 0) {
    // send only the copyup request with a blank snapshot context so that
    // all snapshots are detected from the parent for this object.  If
    // this is a CoW request, a second request will be created for the
    // actual modification.
    ldout(cct, 20) << "copyup with empty snapshot context" << dendl;

    auto comp = util::create_rados_callback<
      CopyupRequest<I>, &CopyupRequest<I>::handle_copyup>(this);
    r = m_image_ctx->data_ctx.aio_operate(
      m_oid, comp, &copyup_op, 0, snaps,
      (m_trace.valid() ? m_trace.get_info() : nullptr));
    ceph_assert(r == 0);
    comp->release();
  }

  if (write_op.size() > 0) {
    // compare-and-write doesn't add any write ops (copyup+cmpext+write
    // can't be executed in the same RADOS op because, unless the object
    // was already present in the clone, cmpext wouldn't see it)
    ldout(cct, 20) << (!deep_copyup && write_op.size() > 2 ?
                        "copyup + ops" : !deep_copyup ? "copyup" : "ops")
                   << " with current snapshot context" << dendl;

    snaps.insert(snaps.end(), snapc.snaps.begin(), snapc.snaps.end());
    auto comp = util::create_rados_callback<
      CopyupRequest<I>, &CopyupRequest<I>::handle_copyup>(this);
    r = m_image_ctx->data_ctx.aio_operate(
      m_oid, comp, &write_op, snapc.seq, snaps,
      (m_trace.valid() ? m_trace.get_info() : nullptr));
    ceph_assert(r == 0);
    comp->release();
  }
}

template <typename I>
void CopyupRequest<I>::handle_copyup(int r) {
  auto cct = m_image_ctx->cct;
  unsigned pending_copyups;
  {
    Mutex::Locker locker(m_lock);
    ceph_assert(m_pending_copyups > 0);
    pending_copyups = --m_pending_copyups;
    // Capture the first error seen; subsequent completions may carry different
    // (or zero) return codes, so we must not overwrite a stored error.
    if (r < 0 && r != -ENOENT && m_result == 0) {
      m_result = r;
    }
  }

  ldout(cct, 20) << "oid=" << m_oid << ", r=" << r
                 << ", pending=" << pending_copyups << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(cct) << "failed to copyup object: " << cpp_strerror(r) << dendl;
  }

  if (pending_copyups == 0) {
    // All copyup operations complete.  Check the aggregate result, not just
    // the last callback's r: one op might have failed while another succeeded.
    if (m_result < 0) {
      // Fail all waiters with the real error.  Do NOT call finish() here —
      // that would call complete_requests() a second time via finish()'s body
      // and then attempt 'delete this' twice.
      complete_requests(false, m_result);
      m_cancelled->store(true);
      delete this;
    } else {
      fire_parent_s3_writeback();
      finish(0);
    }
  }
}

template <typename I>
void CopyupRequest<I>::finish(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 20) << "oid=" << m_oid << ", r=" << r << dendl;

  complete_requests(true, r);
  m_cancelled->store(true);
  delete this;
}

template <typename I>
void CopyupRequest<I>::complete_requests(bool override_restart_retval, int r) {
  auto cct = m_image_ctx->cct;
  remove_from_list();

  while (!m_pending_requests.empty()) {
    auto it = m_pending_requests.begin();
    auto req = *it;
    ldout(cct, 20) << "completing request " << req << dendl;
    req->handle_copyup(r);
    m_pending_requests.erase(it);
  }

  if (override_restart_retval) {
    r = -ERESTART;
  }

  while (!m_restart_requests.empty()) {
    auto it = m_restart_requests.begin();
    auto req = *it;
    ldout(cct, 20) << "restarting request " << req << dendl;
    req->handle_copyup(r);
    m_restart_requests.erase(it);
  }
}

template <typename I>
void CopyupRequest<I>::disable_append_requests() {
  ceph_assert(m_lock.is_locked());
  m_append_request_permitted = false;
}

template <typename I>
void CopyupRequest<I>::remove_from_list() {
  Mutex::Locker copyup_list_locker(m_image_ctx->copyup_list_lock);

  auto it = m_image_ctx->copyup_list.find(m_object_no);
  if (it != m_image_ctx->copyup_list.end()) {
    m_image_ctx->copyup_list.erase(it);
  }
}

template <typename I>
bool CopyupRequest<I>::is_copyup_required() {
  ceph_assert(m_lock.is_locked());

  bool copy_on_read = m_pending_requests.empty();
  if (copy_on_read) {
    // always force a copyup if CoR enabled
    return true;
  }

  if (!m_copyup_is_zero) {
    return true;
  }

  for (auto req : m_pending_requests) {
    if (!req->is_empty_write_op()) {
      return true;
    }
  }
  return false;
}

template <typename I>
bool CopyupRequest<I>::is_deep_copy() const {
  ceph_assert(m_image_ctx->snap_lock.is_locked());
  return !m_image_ctx->migration_info.empty();
}

template <typename I>
bool CopyupRequest<I>::is_update_object_map_required(int r) {
  ceph_assert(m_image_ctx->snap_lock.is_locked());

  if (r < 0) {
    return false;
  }

  if (m_image_ctx->object_map == nullptr) {
    return false;
  }

  if (m_image_ctx->migration_info.empty()) {
    // migration might have completed while IO was in-flight,
    // assume worst-case and perform an object map update
    return true;
  }

  auto it = m_image_ctx->migration_info.snap_map.find(CEPH_NOSNAP);
  ceph_assert(it != m_image_ctx->migration_info.snap_map.end());
  return it->second[0] != CEPH_NOSNAP;
}

template <typename I>
void CopyupRequest<I>::compute_deep_copy_snap_ids() {
  ceph_assert(m_image_ctx->snap_lock.is_locked());

  // don't copy ids for the snaps updated by object deep copy or
  // that don't overlap
  std::set<uint64_t> deep_copied;
  for (auto &it : m_image_ctx->migration_info.snap_map) {
    if (it.first != CEPH_NOSNAP) {
      deep_copied.insert(it.second.front());
    }
  }

  RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
  std::copy_if(m_image_ctx->snaps.rbegin(), m_image_ctx->snaps.rend(),
               std::back_inserter(m_snap_ids),
               [this, cct=m_image_ctx->cct, &deep_copied](uint64_t snap_id) {
      if (deep_copied.count(snap_id)) {
        m_first_snap_is_clean = true;
        return false;
      }

      uint64_t parent_overlap = 0;
      int r = m_image_ctx->get_parent_overlap(snap_id, &parent_overlap);
      if (r < 0) {
        ldout(cct, 5) << "failed getting parent overlap for snap_id: "
                      << snap_id << ": " << cpp_strerror(r) << dendl;
      }
      if (parent_overlap == 0) {
        return false;
      }
      std::vector<std::pair<uint64_t, uint64_t>> extents;
      Striper::extent_to_file(cct, &m_image_ctx->layout,
                              m_object_no, 0,
                              m_image_ctx->layout.object_size,
                              extents);
      auto overlap = m_image_ctx->prune_parent_extents(
          extents, parent_overlap);
      return overlap > 0;
    });
}

template <typename I>
bool CopyupRequest<I>::should_fetch_from_s3() {
  // Caller MUST already hold parent_lock (at least read mode).
  // We must NOT re-acquire it here: RWLock::RLocker is non-recursive and will
  // deadlock if a writer is waiting (PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP).
  auto cct = m_image_ctx->cct;

  if (!m_image_ctx->s3_fetch_enabled) {
    ldout(cct, 20) << "S3 fetch disabled by config" << dendl;
    return false;
  }

  // parent_lock is held by caller (read_from_parent)
  if (m_image_ctx->parent == nullptr) {
    return false;
  }

  if (m_image_ctx->parent_md.parent_type != PARENT_TYPE_STANDALONE &&
      m_image_ctx->parent_md.parent_type != PARENT_TYPE_REMOTE_STANDALONE) {
    return false;
  }

  if (!m_image_ctx->parent->s3_config.is_valid()) {
    ldout(cct, 20) << "parent S3 config invalid or missing" << dendl;
    return false;
  }

  ldout(cct, 10) << "S3 back-fill enabled for object " << m_object_no << dendl;
  return true;
}

template <typename I>
void CopyupRequest<I>::check_parent_object_exists(std::string parent_oid,
                                                   librados::IoCtx parent_ioctx) {
  auto cct = m_image_ctx->cct;
  // parent_lock is NOT held here — caller extracted parent_oid and parent_ioctx
  // while holding the lock, then released it before this async call.

  ldout(cct, 15) << "checking existence of parent object: " << parent_oid << dendl;

  using klass = CopyupRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_check_parent_object_exists>(this);

  int r = parent_ioctx.aio_stat(parent_oid, rados_completion, nullptr, nullptr);
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void CopyupRequest<I>::handle_check_parent_object_exists(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "parent object existence check result: r=" << r << dendl;

  if (r == -ENOENT) {
    // Object doesn't exist in RADOS, fetch from S3.  No cls_lock on the
    // critical path — the natural RADOS-first read (via this stat) already
    // gives us cross-process dedup: if any peer (backfill, prior writer)
    // populated the parent oid before us, the stat returned 0 and we
    // fall through to read_from_parent(true) instead.
    ldout(cct, 15) << "parent object not in RADOS, fetching from S3" << dendl;
    fetch_from_s3();
  } else {
    if (r < 0) {
      // Stat failed for a reason other than ENOENT.  Unexpected
      // (e.g. permissions, RADOS connectivity); log at error level for
      // operator diagnosis, then fall back to a normal read.
      lderr(cct) << "parent object stat failed: " << cpp_strerror(r)
                 << ", falling back to normal read" << dendl;
    } else {
      ldout(cct, 15) << "parent object exists in RADOS, reading normally" << dendl;
    }
    // skip_s3_check==true: stat already decided this is a normal RADOS read.
    read_from_parent(/*skip_s3_check=*/true);
  }
}

template <typename I>
void CopyupRequest<I>::fetch_from_s3() {
  auto cct = m_image_ctx->cct;
  ldout(cct, 10) << "starting S3 fetch for object " << m_object_no << dendl;

  RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
  if (m_image_ctx->parent == nullptr) {
    ldout(cct, 5) << "parent detached during S3 fetch" << dendl;
    finish(-ENOENT);
    return;
  }

  // Copy s3_config and grab the shared fetcher BEFORE releasing parent_lock.
  // A concurrent flatten or close can detach the parent ImageCtx once the
  // lock is released, making any reference into m_image_ctx->parent a
  // dangling pointer.  The shared_ptr keeps the fetcher alive independently.
  S3Config s3_config = m_image_ctx->parent->s3_config;

  // Validate S3 configuration
  if (!s3_config.is_valid()) {
    lderr(cct) << "invalid S3 configuration" << dendl;
    finish(-EINVAL);
    return;
  }

  // Lazily create and cache one S3ObjectFetcher per parent ImageCtx.
  // All concurrent CopyupRequests for the same parent share this instance,
  // which means they share the curl connection pool (via the CURLSH* inside
  // the fetcher).  HTTP keep-alive connections are reused across COW requests
  // targeting the same S3 endpoint instead of paying TCP+TLS setup each time.
  //
  // Double-checked init: parent_locker is an RWLock R-locker (multiple
  // CopyupRequests can hold it concurrently).  Mutating
  // parent->s3_fetcher under only an R-lock raced non-atomic shared_ptr
  // writes (two threads pass the null check, both make_shared, last
  // write wins and the other shared_ptr leaks its refcount — UB).
  // Mirror ObjectReadRequest's pattern: take the PARENT's snap_lock W
  // for the slow path, re-check under W, then drop W.  Lock ordering:
  // child.parent_lock(R) → parent.snap_lock(W); safe because nothing
  // takes them in the reverse order (parent doesn't reach into its
  // children's parent_lock).
  m_s3_fetcher = m_image_ctx->parent->s3_fetcher;
  if (!m_s3_fetcher) {
    RWLock::WLocker parent_snap_wlocker(m_image_ctx->parent->snap_lock);
    if (!m_image_ctx->parent->s3_fetcher) {
      m_image_ctx->parent->s3_fetcher = std::make_shared<S3ObjectFetcher>(
          cct, s3_config, m_image_ctx->parent->get_object_size());
    }
    m_s3_fetcher = m_image_ctx->parent->s3_fetcher;
  }
  // m_s3_fetcher now holds an independent shared ref so the fetcher
  // stays alive even if the parent is detached (flatten) before our
  // async fetch completes.

  std::string s3_url = s3_config.build_url();

  // Calculate byte range based on object number.
  // Must use the *parent's* object size: the child may have a different order,
  // and the raw S3 image is laid out according to the parent's block size.
  uint64_t object_size = m_image_ctx->parent->get_object_size();
  uint64_t byte_start = m_object_no * object_size;
  uint64_t byte_length = object_size;

  ldout(cct, 10) << "S3 URL: " << s3_url
                 << ", fetching range: bytes=" << byte_start
                 << "-" << (byte_start + byte_length - 1) << dendl;

  parent_locker.unlock();

  // Guard the callback: the detached pthread calls on_finish after delete-this,
  // so check m_cancelled before invoking handle_s3_fetch on freed memory.
  auto cancelled = m_cancelled;
  auto ctx = new FunctionContext([this, cancelled](int r) {
    if (cancelled->load()) return;
    handle_s3_fetch(r);
  });

  m_s3_fetcher->fetch_url(s3_url, &m_s3_data, ctx, byte_start, byte_length,
                          m_cancelled);  // shared ownership keeps flag alive
}

template <typename I>
void CopyupRequest<I>::handle_s3_fetch(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 10) << "S3 fetch result: r=" << r << ", bytes=" << m_s3_data.length() << dendl;

  bool eof_block = false;
  if (r == -EINVAL) {
    // 416 Range Not Satisfiable: the S3 object is shorter than the parent image
    // (e.g., the raw export was truncated, or the block is beyond EOF).
    // Treat as an all-zero block — the same semantics as a sparse RADOS object.
    ldout(cct, 10) << "S3 range beyond EOF (416), treating block "
                   << m_object_no << " as zero" << dendl;
    m_s3_data.clear();
    r = 0;
    eof_block = true;
  }

  if (r < 0) {
    lderr(cct) << "failed to fetch object from S3: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  // Warn only when the server returned 200/206 but sent zero bytes — that
  // is unexpected and likely indicates a misconfigured S3 image.  Do NOT
  // warn for eof_block (416): that is the normal sparse-tail case.
  if (m_s3_data.length() == 0 && !eof_block) {
    ldout(cct, 5) << "warning: S3 fetch succeeded but returned 0 bytes "
                  << "(check S3 image size vs RBD image size)" << dendl;
  }

  ldout(cct, 10) << "successfully fetched " << m_s3_data.length()
                 << " bytes from S3" << dendl;

  // No cls_lock to release — the cross-process write dedup now lives
  // inside the AsyncWritebackThrottler's WritebackRequest, fired from
  // fire_parent_s3_writeback below.  The data flows into the child copyup
  // first; the throttler populates the parent cache off the critical
  // path so the client write returns without waiting for parent RADOS.

  // Opportunistic readahead on the COW path: spawn N reads against the
  // PARENT image so the next K parent objects land in the parent's
  // RADOS cache (and the LRU as a side effect of read_from_s3).  Going
  // through ObjectReadRequest::spawn_prefetch -- rather than
  // S3ObjectFetcher::prefetch_next -- is required for cold->warm
  // correctness; see the design note in ObjectRequest.cc's
  // handle_read_from_s3 readahead loop.
  uint64_t readahead = cct->_conf.template get_val<uint64_t>(
      "rbd_s3_readahead_objects");
  if (readahead > 0 && m_image_ctx->parent != nullptr) {
    for (uint64_t i = 1; i <= readahead; i++) {
      librbd::io::ObjectReadRequest<librbd::ImageCtx>::spawn_prefetch(
          m_image_ctx->parent, m_object_no + i);
    }
  }

  // Transfer S3 data into the copyup buffer (move avoids copying 4MB).
  m_copyup_data = std::move(m_s3_data);
  m_data_is_from_s3 = true;

  // Continue with normal copyup flow
  m_image_ctx->snap_lock.get_read();
  m_lock.Lock();
  m_copyup_is_zero = m_copyup_data.is_zero();
  m_copyup_required = is_copyup_required();
  disable_append_requests();

  if (!m_copyup_required) {
    m_lock.Unlock();
    m_image_ctx->snap_lock.put_read();
    ldout(cct, 20) << "copyup not required after S3 fetch" << dendl;
    finish(0);
    return;
  }

  // Copyup is required - populate snapshot IDs if data is not all zeros
  if (!m_copyup_is_zero) {
    m_snap_ids.insert(m_snap_ids.end(), m_image_ctx->snaps.rbegin(),
                      m_image_ctx->snaps.rend());
  }

  m_lock.Unlock();
  m_image_ctx->snap_lock.put_read();

  // Continue to update object maps and then copyup
  update_object_maps();
}

template <typename I>
void CopyupRequest<I>::fire_parent_s3_writeback() {
  auto cct = m_image_ctx->cct;

  // Capture parent state under the child locks (parent_image_ctx is
  // guaranteed live while we hold snap_lock + parent_lock).  We only
  // need parent_oid, parent_ioctx, and parent_image_id for the
  // throttler submission; the in-memory object_map mutation also runs
  // here so the cached parent map reflects the writeback even if the
  // throttler drops the RADOS persistence under load.
  std::string parent_oid;
  std::string parent_image_id;
  librados::IoCtx parent_ioctx;

  {
    RWLock::RLocker snap_locker(m_image_ctx->snap_lock);
    RWLock::RLocker parent_locker(m_image_ctx->parent_lock);

    if (m_image_ctx->parent == nullptr ||
        (m_image_ctx->parent_md.parent_type != PARENT_TYPE_STANDALONE &&
         m_image_ctx->parent_md.parent_type != PARENT_TYPE_REMOTE_STANDALONE) ||
        !m_data_is_from_s3 ||
        m_copyup_data.length() == 0) {
      return;
    }

    auto parent_image_ctx = m_image_ctx->parent;
    parent_oid      = parent_image_ctx->get_object_name(m_object_no);
    parent_ioctx    = parent_image_ctx->data_ctx;
    parent_image_id = parent_image_ctx->id;

    // In-memory parent object_map mutation: keep the cached bitmap in
    // sync so readers of the parent's in-memory map see OBJECT_EXISTS
    // without waiting for the throttler's RADOS cls write.  Independent
    // of the throttler — if the submission below drops, the in-memory
    // map is mildly optimistic (says EXISTS when RADOS doesn't yet);
    // worst case the next reader pays an extra S3 fetch, no correctness
    // hit.
    if (parent_image_ctx->test_features(RBD_FEATURE_OBJECT_MAP) &&
        parent_image_ctx->object_map != nullptr) {
      RWLock::RLocker owner_locker(parent_image_ctx->owner_lock);
      RWLock::RLocker parent_snap_locker(parent_image_ctx->snap_lock);
      if (parent_image_ctx->object_map != nullptr) {
        RWLock::WLocker om_locker(parent_image_ctx->object_map_lock);
        (*parent_image_ctx->object_map)[m_object_no] = OBJECT_EXISTS;
      }
    }
  }  // all child + parent locks released here

  ldout(cct, 10) << "submitting detached parent-cache writeback for object "
                 << m_object_no << " (" << m_copyup_data.length()
                 << " bytes)" << dendl;

  // Detached writeback via the per-process throttler.  The WritebackRequest
  // acquires the cls_lock sentinel, writes the parent oid, updates the
  // parent's object_map on RADOS, and releases — all off the client
  // critical path.  When a peer (another CopyupRequest, the backfill
  // daemon) holds the lock, the throttler's SM EBUSY-drops silently —
  // that's the cross-process write dedup.  If the throttler is at its
  // in_flight or bytes_in_flight cap, the submission is dropped; the
  // backfill daemon rescan or the next reader will populate the parent.
  //
  // Move m_copyup_data into the throttler: this CopyupRequest is about
  // to finish(0) and self-destruct, so we never read m_copyup_data again
  // after this point.  Moving avoids a 4 MB copy.
  auto& throttler = AsyncWritebackThrottler::instance(cct);
  uint64_t bytes = m_copyup_data.length();
  bool submitted = throttler.try_submit(
      parent_ioctx, parent_oid, m_object_no,
      std::move(m_copyup_data), parent_image_id);
  if (!submitted) {
    ldout(cct, 10) << "throttler full; dropping parent cache populate for "
                   << parent_oid << " (" << bytes << " bytes)" << dendl;
  }
}

} // namespace io
} // namespace librbd

template class librbd::io::CopyupRequest<librbd::ImageCtx>;
