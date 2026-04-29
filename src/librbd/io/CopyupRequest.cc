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
    // Object doesn't exist in RADOS, fetch from S3
    ldout(cct, 15) << "parent object not in RADOS, fetching from S3" << dendl;
    fetch_from_s3_with_lock();
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
void CopyupRequest<I>::fetch_from_s3_with_lock() {
  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "attempting to lock parent object for S3 fetch" << dendl;

  // Store parent IoCtx and OID for later use
  RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
  if (m_image_ctx->parent == nullptr) {
    ldout(cct, 5) << "parent detached during S3 fetch setup" << dendl;
    finish(-ENOENT);
    return;
  }

  m_parent_ioctx = m_image_ctx->parent->data_ctx;
  m_parent_oid = m_image_ctx->parent->get_object_name(m_object_no);
  // Use a separate sentinel object for the cls lock so that acquiring the lock
  // does not create the data object as an empty side effect.  If the lock were
  // placed on m_parent_oid itself, a concurrent child would see the object as
  // "existing" (stat returns 0) but read zero bytes, producing corrupt copyup
  // data for any partial write that relied on the non-zero parent content.
  m_parent_lock_oid = m_parent_oid + S3_FETCH_LOCK_SENTINEL_SUFFIX;

  ldout(cct, 15) << "parent oid: " << m_parent_oid
                 << ", lock oid: " << m_parent_lock_oid << dendl;

  // Construct lock cookie (unique per child instance and object) — cached
  // as m_lock_cookie so unlock_parent_object() doesn't need to recompute it.
  if (m_lock_cookie.empty()) {
    m_lock_cookie = m_image_ctx->id + "_" + stringify(m_object_no);
  }

  // Attempt to acquire exclusive lock on parent object
  // This prevents multiple children from fetching the same object from S3
  librados::ObjectWriteOperation lock_op;

  // Using cls_lock for distributed locking
  // Lock name: "s3_fetch_lock", type: LOCK_EXCLUSIVE
  // Duration: 30 seconds (auto-expires if holder crashes)
  uint32_t lock_timeout = m_image_ctx->s3_parent_lock_timeout;

  // Note: cls::lock::lock() expects utime_t for duration
  utime_t lock_duration(lock_timeout, 0);

  rados::cls::lock::lock(&lock_op, S3_FETCH_LOCK_NAME, LOCK_EXCLUSIVE,
                        m_lock_cookie, S3_FETCH_LOCK_TAG, "S3 fetch in progress",
                        lock_duration, 0);

  using klass = CopyupRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_lock_parent_object>(this);

  int r = m_parent_ioctx.aio_operate(m_parent_lock_oid, rados_completion, &lock_op);
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void CopyupRequest<I>::handle_lock_parent_object(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "lock result: r=" << r << dendl;

  if (r == 0) {
    // Lock acquired successfully!
    m_s3_lock_acquired = true;
    ldout(cct, 10) << "acquired S3 fetch lock, proceeding to fetch from S3" << dendl;

    // Fetch object from S3
    fetch_from_s3_async();

  } else if (r == -EBUSY || r == -EEXIST) {
    // Lock is held by another entity.  Principle: user I/O must never wait
    // for the backfill daemon — the parent will be populated either by the
    // daemon or by this request itself.  Identify the holder and act:
    //
    //  - Backfill daemon (cookie starts with "backfill-"): break its lock
    //    immediately, then re-check parent existence.  If the daemon already
    //    wrote the object we can read from the parent without any S3 round-trip.
    //    If not, we fetch from S3 ourselves.
    //
    //  - Another CopyupRequest (foreground I/O): don't break it, but also
    //    don't apply exponential backoff.  Re-check parent existence right
    //    away; if still absent, proceed without the lock (harmless duplicate
    //    S3 fetch — write_full is idempotent).
    ldout(cct, 10) << "lock busy, checking if backfill daemon holds it" << dendl;
    try_preempt_backfill_lock();

  } else {
    // Lock operation failed for other reason
    lderr(cct) << "failed to acquire S3 fetch lock: " << cpp_strerror(r) << dendl;
    finish(r);
  }
}

template <typename I>
void CopyupRequest<I>::try_preempt_backfill_lock() {
  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "listing lock holders on " << m_parent_lock_oid << dendl;

  // Async RADOS read to list lock holders.
  librados::ObjectReadOperation read_op;
  rados::cls::lock::get_lock_info_start(&read_op, S3_FETCH_LOCK_NAME);

  m_lock_info_bl.clear();
  using klass = CopyupRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_list_lock_holders>(this);

  int r = m_parent_ioctx.aio_operate(
    m_parent_lock_oid, rados_completion, &read_op, &m_lock_info_bl);
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void CopyupRequest<I>::handle_list_lock_holders(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "r=" << r << dendl;

  if (r < 0) {
    ldout(cct, 10) << "failed to list lock holders, falling back to retry: "
                   << cpp_strerror(r) << dendl;
    retry_read_from_parent();
    return;
  }

  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t> lockers;
  ClsLockType lock_type;
  std::string tag;

  auto it = m_lock_info_bl.cbegin();
  r = rados::cls::lock::get_lock_info_finish(&it, &lockers, &lock_type, &tag);
  if (r < 0) {
    ldout(cct, 10) << "failed to parse lock info, falling back to retry: "
                   << cpp_strerror(r) << dendl;
    retry_read_from_parent();
    return;
  }

  for (auto &kv : lockers) {
    const auto &cookie = kv.first.cookie;
    if (boost::starts_with(cookie, BACKFILL_LOCK_COOKIE_PREFIX)) {
      ldout(cct, 10) << "backfill daemon holds lock (cookie=" << cookie
                     << ", entity=" << kv.first.locker
                     << "), breaking to preempt" << dendl;

      librados::ObjectWriteOperation break_op;
      rados::cls::lock::break_lock(&break_op, S3_FETCH_LOCK_NAME,
                                   cookie, kv.first.locker);

      // Wait for the break to complete before re-checking parent existence.
      // If we queued read_from_parent() before the break finished, a
      // concurrent backfill re-acquire could still be in-flight, causing
      // a spurious second -EBUSY.  By using a proper callback the ordering
      // is guaranteed: break → handle_break_backfill_lock → read_from_parent.
      using klass = CopyupRequest<I>;
      librados::AioCompletion *break_completion =
        util::create_rados_callback<klass, &klass::handle_break_backfill_lock>(this);
      int br = m_parent_ioctx.aio_operate(
        m_parent_lock_oid, break_completion, &break_op);
      ceph_assert(br == 0);
      break_completion->release();
      return;
    }
  }

  // No backfill daemon found — lock is held by another foreground user
  // request.  Do NOT preempt (user vs user is cooperative on the writeback,
  // not the fetch): fetch our own S3 copy in parallel and skip the
  // parent-RADOS writeback.  The lock holder is already committed to
  // populating the parent object; us writing too would just spend duplicate
  // RADOS IOPS on a write_full that the holder will issue anyway.  Setting
  // m_skip_parent_writeback short-circuits fire_parent_s3_writeback() in the
  // copyup completion path while leaving the in-memory data flow intact.
  ldout(cct, 10) << "lock held by another user request, fetching own copy "
                 << "and skipping writeback (peer will populate parent)" << dendl;
  m_skip_parent_writeback = true;
  // No lock to release; fetch_from_s3_async checks m_s3_lock_acquired before
  // unlocking on its error paths, and unlock_parent_object() is a no-op when
  // the flag is false.
  fetch_from_s3_async();
}

template <typename I>
void CopyupRequest<I>::handle_break_backfill_lock(int r) {
  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "break_lock result: r=" << r << dendl;

  // Ignore -ENOENT (lock already released by the daemon) and other errors —
  // the lock sentinel may have auto-expired or the daemon released it between
  // our get_lock_info and our break_lock.  In all cases, proceed to re-check
  // whether the daemon wrote the parent object before or during the break.
  if (r < 0 && r != -ENOENT) {
    ldout(cct, 5) << "break_lock returned " << cpp_strerror(r)
                  << ", proceeding to re-check parent anyway" << dendl;
  }

  // Re-enter from read_from_parent() which re-stats the parent object.
  // If the daemon wrote it before we broke the lock, we read from parent
  // without any S3 round-trip.  If not, fetch_from_s3_with_lock() re-tries
  // the lock acquisition (which should now succeed since we broke it) or
  // proceeds without the lock if another request re-acquired it.
  //
  // Queue via op_work_queue (same pattern as handle_list_lock_holders) so that
  // a concurrent finish()/delete-this on another path cannot use-after-free
  // `this` before the lambda executes.
  ldout(cct, 10) << "re-checking parent after breaking backfill lock" << dendl;
  auto cancelled = m_cancelled;
  m_image_ctx->op_work_queue->queue(
    new FunctionContext([this, cancelled](int) {
      if (cancelled->load()) return;
      read_from_parent();
    }), 0);
}

// retry_read_from_parent is the fallback path for the rare case where
// get_lock_info_start / get_lock_info_finish fails (RADOS or decode error in
// handle_list_lock_holders).  Normal lock-contention between CopyupRequests is
// handled without backoff via the immediate re-check in handle_list_lock_holders.
// The exponential backoff here is appropriate because a parse error suggests
// a transient OSD or network issue rather than routine contention.
template <typename I>
void CopyupRequest<I>::retry_read_from_parent() {
  auto cct = m_image_ctx->cct;

  m_s3_retry_count++;
  uint32_t max_retries = m_image_ctx->s3_lock_retry_max;

  if (m_s3_retry_count > max_retries) {
    lderr(cct) << "exceeded maximum S3 lock retries (" << max_retries
               << "), giving up" << dendl;
    finish(-ETIMEDOUT);
    return;
  }

  // Exponential backoff: 1s, 2s, 4s, 8s, 16s … capped at 30s.
  // Two guards: (a) cap shift to 20 to avoid undefined 32-bit left-shift;
  // (b) cap delay to 30 000 ms so I/O does not stall for minutes even if
  // rbd_s3_lock_retry_max is set to its maximum value.
  uint32_t shift = (m_s3_retry_count - 1 < 20u) ? (m_s3_retry_count - 1) : 20u;
  uint32_t delay_ms = std::min(1000u * (1u << shift), 30000u);

  ldout(cct, 10) << "retry #" << m_s3_retry_count << " after " << delay_ms
                 << "ms (another child may be writing parent object)" << dendl;

  // Schedule the retry via SafeTimer so we actually wait the computed delay.
  // The timer callback must be fast (runs under timer lock), so it simply
  // re-queues read_from_parent() onto the op_work_queue.
  //
  // CopyupRequest is self-deleting; we pass m_cancelled by shared_ptr so that
  // a stale lambda (fired after delete this in an unexpected edge-case path)
  // checks the flag before accessing any member.  In normal operation no
  // finish() path fires while the timer is pending (m_async_op keeps the
  // image open), but this guard makes the invariant explicit and safe.
  SafeTimer *timer;
  Mutex *timer_lock;
  ImageCtx::get_timer_instance(cct, &timer, &timer_lock);
  {
    // Capture op_work_queue *before* the cancellation check so we never
    // dereference 'this' inside the lambda after the flag may have been set.
    auto wq = m_image_ctx->op_work_queue;
    auto cancelled = m_cancelled;   // capture by value (shared ownership)
    Mutex::Locker locker(*timer_lock);
    timer->add_event_after(delay_ms / 1000.0,
      new FunctionContext([this, cancelled, wq](int r) {
        if (cancelled->load()) return;
        wq->queue(
          new FunctionContext([this, cancelled](int r) {
            if (cancelled->load()) return;
            read_from_parent();
          }), 0);
      }));
  }
}

template <typename I>
void CopyupRequest<I>::fetch_from_s3_async() {
  auto cct = m_image_ctx->cct;
  ldout(cct, 10) << "starting S3 fetch for object " << m_object_no << dendl;

  RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
  if (m_image_ctx->parent == nullptr) {
    ldout(cct, 5) << "parent detached during S3 fetch" << dendl;
    unlock_parent_object();
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
    unlock_parent_object();
    finish(-EINVAL);
    return;
  }

  // Lazily create and cache one S3ObjectFetcher per parent ImageCtx.
  // All concurrent CopyupRequests for the same parent share this instance,
  // which means they share the curl connection pool (via the CURLSH* inside
  // the fetcher).  HTTP keep-alive connections are reused across COW requests
  // targeting the same S3 endpoint instead of paying TCP+TLS setup each time.
  if (!m_image_ctx->parent->s3_fetcher) {
    m_image_ctx->parent->s3_fetcher =
      std::make_shared<S3ObjectFetcher>(cct, s3_config, m_image_ctx->parent->get_object_size());
  }
  // Take a shared ref so the fetcher stays alive even if the parent is
  // detached (flatten) before the async pthread completes.
  m_s3_fetcher = m_image_ctx->parent->s3_fetcher;

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
    unlock_parent_object();
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

  // Release the sentinel lock.  The data will be written back to the parent
  // RADOS object (and the parent's in-memory + on-disk object map updated)
  // by fire_parent_s3_writeback once the child copyup completes via
  // handle_copyup.  Doing it after — not before — the child copyup ensures
  // both the in-memory ObjectMap and the on-disk RADOS cls record are updated
  // under parent_lock through the same callback chain.
  unlock_parent_object();

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
void CopyupRequest<I>::unlock_parent_object() {
  if (!m_s3_lock_acquired) {
    return;
  }

  auto cct = m_image_ctx->cct;
  ldout(cct, 15) << "unlocking parent object" << dendl;

  // Release the lock asynchronously to avoid blocking the I/O path on a
  // RADOS round-trip.  No completion callback: 'this' may be deleted before
  // the callback fires, and CephContext-only logging requires a C-style
  // callback.  Unlock failures are tolerable because the cls_lock auto-expires
  // via the configured timeout, allowing the next holder to acquire the lock
  // once the lease expires.  The initial submission error IS logged below.
  librados::ObjectWriteOperation unlock_op;
  rados::cls::lock::unlock(&unlock_op, S3_FETCH_LOCK_NAME, m_lock_cookie);

  auto rados_completion = librados::Rados::aio_create_completion();
  int r = m_parent_ioctx.aio_operate(m_parent_lock_oid, rados_completion, &unlock_op);
  rados_completion->release();
  if (r < 0) {
    ldout(cct, 5) << "warning: failed to submit async unlock for parent object: "
                  << cpp_strerror(r) << dendl;
  }

  m_s3_lock_acquired = false;
}


template <typename I>
void CopyupRequest<I>::fire_parent_s3_writeback() {
  auto cct = m_image_ctx->cct;

  // The lock holder owns the writeback.  When this request raced a peer
  // user that already held the s3_fetch_lock, we fetched our own copy of
  // the data (returned to the caller via the in-memory bufferlist) but
  // must not write to the parent — the peer will issue its own write_full
  // for that.  Two write_fulls would double the RADOS IOPS for no benefit.
  if (m_skip_parent_writeback) {
    ldout(cct, 10) << "skipping parent writeback: lock holder peer will populate "
                   << "parent object " << m_object_no << dendl;
    return;
  }

  // Capture all state needed for the async ops while holding child locks.
  // Once all locals are populated we release the locks and fire both the
  // write_full and the object-map update as pure fire-and-forget ops — no
  // callback references 'this', so finish(0)/delete this is safe immediately
  // after this function returns.
  std::string parent_oid;
  librados::IoCtx parent_ioctx;
  bool has_object_map = false;
  std::string object_map_name;

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
    parent_oid    = parent_image_ctx->get_object_name(m_object_no);
    parent_ioctx  = parent_image_ctx->data_ctx;
    has_object_map = parent_image_ctx->test_features(RBD_FEATURE_OBJECT_MAP);

    if (has_object_map) {
      object_map_name = ObjectMap<>::object_map_name(
        parent_image_ctx->id, CEPH_NOSNAP);

      // Path 1 (in-memory map): update the bitmap in-memory now, while
      // parent_image_ctx is guaranteed live.  The async RADOS cls write below
      // persists both paths uniformly, so we do not need to call aio_update
      // (which would require a live parent_image_ctx* in a callback).
      if (parent_image_ctx->object_map != nullptr) {
        RWLock::RLocker owner_locker(parent_image_ctx->owner_lock);
        RWLock::RLocker parent_snap_locker(parent_image_ctx->snap_lock);
        if (parent_image_ctx->object_map != nullptr) {
          RWLock::WLocker om_locker(parent_image_ctx->object_map_lock);
          (*parent_image_ctx->object_map)[m_object_no] = OBJECT_EXISTS;
        }
      }
    }
  }  // all child + parent locks released here

  ldout(cct, 10) << "firing async write-back to parent object " << m_object_no
                 << " (" << m_copyup_data.length() << " bytes)" << dendl;

  // Fire write_full — no callback to 'this'
  {
    librados::ObjectWriteOperation write_op;
    write_op.write_full(m_copyup_data);
    auto c = librados::Rados::aio_create_completion();
    int r = parent_ioctx.aio_operate(parent_oid, c, &write_op);
    ceph_assert(r == 0);
    c->release();
  }

  // Fire object-map update — no callback to 'this'
  if (has_object_map) {
    librados::ObjectWriteOperation map_op;
    ObjectMap<>::build_update_op(&map_op, m_object_no, m_object_no + 1,
                                  OBJECT_EXISTS, boost::optional<uint8_t>());
    auto c = librados::Rados::aio_create_completion();
    int r = parent_ioctx.aio_operate(object_map_name, c, &map_op);
    ceph_assert(r == 0);
    c->release();
  }
}

} // namespace io
} // namespace librbd

template class librbd::io::CopyupRequest<librbd::ImageCtx>;
