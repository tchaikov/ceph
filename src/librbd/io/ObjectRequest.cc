// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/ObjectRequest.h"
#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Mutex.h"
#include "common/RWLock.h"
#include "common/Timer.h"
#include "common/WorkQueue.h"
#include "include/Context.h"
#include "include/err.h"
#include "osd/osd_types.h"

#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ObjectMap.h"
#include "librbd/Types.h"
#include "librbd/Utils.h"
#include "librbd/io/S3ObjectFetcher.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/CopyupRequest.h"
#include "librbd/io/ImageRequest.h"
#include "librbd/io/ReadResult.h"
#include "cls/lock/cls_lock_client.h"
#include "cls/lock/cls_lock_types.h"
#include "cls/rbd/cls_rbd_client.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind.hpp>
#include <boost/optional.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::ObjectRequest: " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace io {

namespace {

template <typename I>
inline bool is_copy_on_read(I *ictx, librados::snap_t snap_id) {
  RWLock::RLocker snap_locker(ictx->snap_lock);
  return (ictx->clone_copy_on_read &&
          !ictx->read_only && snap_id == CEPH_NOSNAP &&
          (ictx->exclusive_lock == nullptr ||
           ictx->exclusive_lock->is_lock_owner()));
}

} // anonymous namespace

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_write(I *ictx, const std::string &oid,
                               uint64_t object_no, uint64_t object_off,
                               ceph::bufferlist&& data,
                               const ::SnapContext &snapc, int op_flags,
			       const ZTracer::Trace &parent_trace,
                               Context *completion) {
  return new ObjectWriteRequest<I>(ictx, oid, object_no, object_off,
                                   std::move(data), snapc, op_flags,
                                   parent_trace, completion);
}

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_discard(I *ictx, const std::string &oid,
                                 uint64_t object_no, uint64_t object_off,
                                 uint64_t object_len,
                                 const ::SnapContext &snapc,
                                 int discard_flags,
                                 const ZTracer::Trace &parent_trace,
                                 Context *completion) {
  return new ObjectDiscardRequest<I>(ictx, oid, object_no, object_off,
                                     object_len, snapc, discard_flags,
                                     parent_trace, completion);
}

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_write_same(I *ictx, const std::string &oid,
                                   uint64_t object_no, uint64_t object_off,
                                   uint64_t object_len,
                                   ceph::bufferlist&& data,
                                   const ::SnapContext &snapc, int op_flags,
				   const ZTracer::Trace &parent_trace,
                                   Context *completion) {
  return new ObjectWriteSameRequest<I>(ictx, oid, object_no, object_off,
                                       object_len, std::move(data), snapc,
                                       op_flags, parent_trace, completion);
}

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_compare_and_write(I *ictx, const std::string &oid,
                                           uint64_t object_no,
                                           uint64_t object_off,
                                           ceph::bufferlist&& cmp_data,
                                           ceph::bufferlist&& write_data,
                                           const ::SnapContext &snapc,
                                           uint64_t *mismatch_offset,
                                           int op_flags,
                                           const ZTracer::Trace &parent_trace,
                                           Context *completion) {
  return new ObjectCompareAndWriteRequest<I>(ictx, oid, object_no, object_off,
                                             std::move(cmp_data),
                                             std::move(write_data), snapc,
                                             mismatch_offset, op_flags,
                                             parent_trace, completion);
}

template <typename I>
ObjectRequest<I>::ObjectRequest(I *ictx, const std::string &oid,
                                uint64_t objectno, uint64_t off,
                                uint64_t len, librados::snap_t snap_id,
                                const char *trace_name,
                                const ZTracer::Trace &trace,
				Context *completion)
  : m_ictx(ictx), m_oid(oid), m_object_no(objectno), m_object_off(off),
    m_object_len(len), m_snap_id(snap_id), m_completion(completion),
    m_trace(util::create_trace(*ictx, "", trace)) {
  ceph_assert(m_ictx->data_ctx.is_valid());
  if (m_trace.valid()) {
    m_trace.copy_name(trace_name + std::string(" ") + oid);
    m_trace.event("start");
  }
}

template <typename I>
void ObjectRequest<I>::add_write_hint(I& image_ctx,
                                      librados::ObjectWriteOperation *wr) {
  if (image_ctx.enable_alloc_hint) {
    wr->set_alloc_hint2(image_ctx.get_object_size(),
                        image_ctx.get_object_size(),
                        image_ctx.alloc_hint_flags);
  } else if (image_ctx.alloc_hint_flags != 0U) {
    wr->set_alloc_hint2(0, 0, image_ctx.alloc_hint_flags);
  }
}

template <typename I>
bool ObjectRequest<I>::compute_parent_extents(Extents *parent_extents,
                                              bool read_request) {
  ceph_assert(m_ictx->snap_lock.is_locked());
  ceph_assert(m_ictx->parent_lock.is_locked());

  m_has_parent = false;
  parent_extents->clear();

  uint64_t parent_overlap;
  int r = m_ictx->get_parent_overlap(m_snap_id, &parent_overlap);
  if (r < 0) {
    // NOTE: it's possible for a snapshot to be deleted while we are
    // still reading from it
    lderr(m_ictx->cct) << "failed to retrieve parent overlap: "
                       << cpp_strerror(r) << dendl;
    return false;
  }

  if (!read_request && !m_ictx->migration_info.empty()) {
    parent_overlap = m_ictx->migration_info.overlap;
  }

  if (parent_overlap == 0) {
    return false;
  }

  Striper::extent_to_file(m_ictx->cct, &m_ictx->layout, m_object_no, 0,
                          m_ictx->layout.object_size, *parent_extents);
  uint64_t object_overlap = m_ictx->prune_parent_extents(*parent_extents,
                                                         parent_overlap);
  if (object_overlap > 0) {
    ldout(m_ictx->cct, 20) << "overlap " << parent_overlap << " "
                           << "extents " << *parent_extents << dendl;
    m_has_parent = !parent_extents->empty();
    return true;
  }
  return false;
}

template <typename I>
void ObjectRequest<I>::async_finish(int r) {
  ldout(m_ictx->cct, 20) << "r=" << r << dendl;
  m_ictx->op_work_queue->queue(util::create_context_callback<
    ObjectRequest<I>, &ObjectRequest<I>::finish>(this), r);
}

template <typename I>
void ObjectRequest<I>::finish(int r) {
  ldout(m_ictx->cct, 20) << "r=" << r << dendl;
  m_completion->complete(r);
  delete this;
}

/** read **/

template <typename I>
ObjectReadRequest<I>::ObjectReadRequest(I *ictx, const std::string &oid,
                                        uint64_t objectno, uint64_t offset,
                                        uint64_t len, librados::snap_t snap_id,
                                        int op_flags,
                                        const ZTracer::Trace &parent_trace,
                                        bufferlist* read_data,
                                        ExtentMap* extent_map,
                                        Context *completion)
  : ObjectRequest<I>(ictx, oid, objectno, offset, len, snap_id, "read",
                     parent_trace, completion),
    m_op_flags(op_flags), m_read_data(read_data), m_extent_map(extent_map) {
}

template <typename I>
ObjectReadRequest<I>::~ObjectReadRequest() {
  // Signal any still-pending wait_for_peer_writeback timer lambda to bail
  // before it dereferences `this`.  The lambda captured m_cancelled by value
  // (shared_ptr) so the flag remains valid even after this object is freed.
  m_cancelled->store(true);
}

template <typename I>
void ObjectReadRequest<I>::send() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  read_object();
}

template <typename I>
void ObjectReadRequest<I>::read_object() {
  I *image_ctx = this->m_ictx;
  {
    RWLock::RLocker snap_locker(image_ctx->snap_lock);
    if (image_ctx->object_map != nullptr &&
        !image_ctx->object_map->object_may_exist(this->m_object_no)) {
      image_ctx->op_work_queue->queue(new FunctionContext([this](int r) {
          read_parent();
        }), 0);
      return;
    }
  }

  ldout(image_ctx->cct, 20) << dendl;

  librados::ObjectReadOperation op;
  if (this->m_object_len >= image_ctx->sparse_read_threshold_bytes) {
    op.sparse_read(this->m_object_off, this->m_object_len, m_extent_map,
                   m_read_data, nullptr);
  } else {
    op.read(this->m_object_off, this->m_object_len, m_read_data, nullptr);
  }
  op.set_op_flags2(m_op_flags);

  librados::AioCompletion *rados_completion = util::create_rados_callback<
    ObjectReadRequest<I>, &ObjectReadRequest<I>::handle_read_object>(this);
  int flags = image_ctx->get_read_flags(this->m_snap_id);
  int r = image_ctx->data_ctx.aio_operate(
    this->m_oid, rados_completion, &op, flags, nullptr,
    (this->m_trace.valid() ? this->m_trace.get_info() : nullptr));
  ceph_assert(r == 0);

  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_read_object(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT) {
    if (should_read_from_s3()) {
      read_from_s3_with_lock();
      return;
    }

    read_parent();
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to read from object: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  this->finish(0);
}

template <typename I>
void ObjectReadRequest<I>::read_parent() {
  I *image_ctx = this->m_ictx;

  RWLock::RLocker snap_locker(image_ctx->snap_lock);
  RWLock::RLocker parent_locker(image_ctx->parent_lock);

  // calculate reverse mapping onto the image
  Extents parent_extents;
  Striper::extent_to_file(image_ctx->cct, &image_ctx->layout,
                          this->m_object_no, this->m_object_off,
                          this->m_object_len, parent_extents);

  uint64_t parent_overlap = 0;
  uint64_t object_overlap = 0;
  int r = image_ctx->get_parent_overlap(this->m_snap_id, &parent_overlap);
  if (r == 0) {
    object_overlap = image_ctx->prune_parent_extents(parent_extents,
                                                     parent_overlap);
  }

  if (object_overlap == 0) {
    parent_locker.unlock();
    snap_locker.unlock();

    this->finish(-ENOENT);
    return;
  }

  ldout(image_ctx->cct, 20) << dendl;

  auto parent_completion = AioCompletion::create_and_start<
    ObjectReadRequest<I>, &ObjectReadRequest<I>::handle_read_parent>(
      this, util::get_image_ctx(image_ctx->parent), AIO_TYPE_READ);
  ImageRequest<I>::aio_read(image_ctx->parent, parent_completion,
                            std::move(parent_extents), ReadResult{m_read_data},
                            0, this->m_trace);
}

template <typename I>
void ObjectReadRequest<I>::handle_read_parent(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT) {
    this->finish(r);
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to read parent extents: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  copyup();
}

template <typename I>
void ObjectReadRequest<I>::copyup() {
  I *image_ctx = this->m_ictx;
  if (!is_copy_on_read(image_ctx, this->m_snap_id)) {
    this->finish(0);
    return;
  }

  image_ctx->owner_lock.get_read();
  image_ctx->snap_lock.get_read();
  image_ctx->parent_lock.get_read();
  Extents parent_extents;
  if (!this->compute_parent_extents(&parent_extents, true) ||
      (image_ctx->exclusive_lock != nullptr &&
       !image_ctx->exclusive_lock->is_lock_owner())) {
    image_ctx->parent_lock.put_read();
    image_ctx->snap_lock.put_read();
    image_ctx->owner_lock.put_read();
    this->finish(0);
    return;
  }

  ldout(image_ctx->cct, 20) << dendl;

  image_ctx->copyup_list_lock.Lock();
  auto it = image_ctx->copyup_list.find(this->m_object_no);
  if (it == image_ctx->copyup_list.end()) {
    // create and kick off a CopyupRequest
    auto new_req = CopyupRequest<I>::create(
      image_ctx, this->m_oid, this->m_object_no, std::move(parent_extents),
      this->m_trace);

    image_ctx->copyup_list[this->m_object_no] = new_req;
    image_ctx->copyup_list_lock.Unlock();
    image_ctx->parent_lock.put_read();
    image_ctx->snap_lock.put_read();
    new_req->send();
  } else {
    image_ctx->copyup_list_lock.Unlock();
    image_ctx->parent_lock.put_read();
    image_ctx->snap_lock.put_read();
  }

  image_ctx->owner_lock.put_read();
  this->finish(0);
}

template <typename I>
bool ObjectReadRequest<I>::should_read_from_s3() {
  I *image_ctx = this->m_ictx;

  // Fast path: kill-switch checked without any lock overhead.
  if (!image_ctx->s3_fetch_enabled) {
    return false;
  }

  RWLock::RLocker snap_locker(image_ctx->snap_lock);

  // Fetch from S3 if this image has S3 backend configured.
  // This works correctly for both cases:
  // 1. Child (regular or standalone) reading: child has no s3_config → returns false → calls read_parent()
  // 2. S3-backed parent reading: parent has s3_config → returns true → fetches from S3
  // 3. Regular snapshot parent reading: parent has no s3_config → returns false → calls read_parent() (will fail -ENOENT, expected)

  if (!image_ctx->s3_config.is_valid()) {
    ldout(image_ctx->cct, 20) << "no valid S3 backend configured" << dendl;
    return false;
  }

  ldout(image_ctx->cct, 10) << "S3-backed image, will fetch from S3" << dendl;
  return true;
}

template <typename I>
void ObjectReadRequest<I>::read_from_s3_with_lock() {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "attempting to lock object for S3 read: " << this->m_oid << dendl;

  // Sentinel oid: locking the data oid directly would make stat() return 0
  // (object exists, length 0) and trick a peer's "is the object populated?"
  // check, so place the lock on a sibling object.  Same convention as
  // CopyupRequest::fetch_from_s3_with_lock; both paths coordinate on the
  // same sentinel oid for cross-process dedup against backfill.
  m_lock_oid = this->m_oid + S3_FETCH_LOCK_SENTINEL_SUFFIX;
  if (m_lock_cookie.empty()) {
    // "_r_" marker distinguishes read-side cookies from CopyupRequest's
    // copyup-side cookies — useful when listing lock holders for diagnostics.
    m_lock_cookie = image_ctx->id + "_r_" + stringify(this->m_object_no);
  }

  uint32_t lock_timeout = image_ctx->s3_parent_lock_timeout;
  utime_t lock_duration(lock_timeout, 0);

  librados::ObjectWriteOperation lock_op;
  rados::cls::lock::lock(&lock_op, S3_FETCH_LOCK_NAME, LOCK_EXCLUSIVE,
                         m_lock_cookie, S3_FETCH_LOCK_TAG, "S3 read in progress",
                         lock_duration, 0);

  using klass = ObjectReadRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_lock_for_s3_read>(this);

  int r = image_ctx->data_ctx.aio_operate(m_lock_oid, rados_completion, &lock_op);
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_lock_for_s3_read(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "lock result: r=" << r << dendl;

  if (r == 0) {
    ceph_assert(!m_skip_writeback);
    m_s3_lock_acquired = true;
    ldout(cct, 10) << "acquired S3 read lock, re-stat'ing object before fetch" << dendl;
    recheck_oid_after_lock();
  } else if (r == -EBUSY || r == -EEXIST) {
    ldout(cct, 10) << "lock busy, identifying holder" << dendl;
    try_preempt_backfill_lock_for_read();
  } else {
    // Don't fail user IO on coordination errors — proceed with own fetch.
    // Skip writeback as a precaution since we don't know the lock state.
    ldout(cct, 5) << "failed to acquire S3 read lock: " << cpp_strerror(r)
                  << ", proceeding with own fetch (skip writeback)" << dendl;
    ceph_assert(!m_s3_lock_acquired);
    m_skip_writeback = true;
    read_from_s3();
  }
}

template <typename I>
void ObjectReadRequest<I>::try_preempt_backfill_lock_for_read() {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "listing lock holders on " << m_lock_oid << dendl;

  librados::ObjectReadOperation read_op;
  rados::cls::lock::get_lock_info_start(&read_op, S3_FETCH_LOCK_NAME);

  m_lock_info_bl.clear();
  using klass = ObjectReadRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_list_lock_holders_for_read>(this);

  int r = image_ctx->data_ctx.aio_operate(
    m_lock_oid, rados_completion, &read_op, &m_lock_info_bl);
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_list_lock_holders_for_read(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "r=" << r << dendl;

  if (r < 0) {
    // Match CopyupRequest::handle_list_lock_holders: same event, same level.
    ldout(cct, 10) << "failed to list lock holders, falling back to own fetch: "
                   << cpp_strerror(r) << dendl;
    ceph_assert(!m_s3_lock_acquired);
    m_skip_writeback = true;
    read_from_s3();
    return;
  }

  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t> lockers;
  ClsLockType lock_type;
  std::string tag;

  auto it = m_lock_info_bl.cbegin();
  r = rados::cls::lock::get_lock_info_finish(&it, &lockers, &lock_type, &tag);
  if (r < 0) {
    ldout(cct, 10) << "failed to parse lock info, falling back to own fetch: "
                   << cpp_strerror(r) << dendl;
    ceph_assert(!m_s3_lock_acquired);
    m_skip_writeback = true;
    read_from_s3();
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

      using klass = ObjectReadRequest<I>;
      librados::AioCompletion *break_completion =
        util::create_rados_callback<klass, &klass::handle_break_backfill_lock_for_read>(this);
      int br = image_ctx->data_ctx.aio_operate(
        m_lock_oid, break_completion, &break_op);
      ceph_assert(br == 0);
      break_completion->release();
      return;
    }
  }

  // No backfill holder — another foreground user request holds the lock.
  // User-vs-user is cooperative on the writeback only: we fetch our own
  // S3 copy in parallel (no waiting on the holder) but skip the RADOS
  // write_full + object_map update so the holder populates the object
  // exactly once.  Two write_fulls would cost duplicate IOPS without
  // changing the resulting bytes — RBD parents are immutable raw exports.
  ldout(cct, 10) << "lock held by another user request, fetching own copy + "
                 << "skipping writeback (peer will populate)" << dendl;
  ceph_assert(!m_s3_lock_acquired);
  m_skip_writeback = true;
  read_from_s3();
}

template <typename I>
void ObjectReadRequest<I>::handle_break_backfill_lock_for_read(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "break_lock result: r=" << r << dendl;

  // -ENOENT means the daemon released between our list and our break;
  // any other error is logged but not fatal — re-attempting the acquire
  // is the right next step in either case.
  if (r < 0 && r != -ENOENT) {
    ldout(cct, 5) << "break_lock returned " << cpp_strerror(r)
                  << ", retrying lock acquire anyway" << dendl;
  }
  read_from_s3_with_lock();
}

template <typename I>
void ObjectReadRequest<I>::recheck_oid_after_lock() {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "stat'ing " << this->m_oid
                 << " after lock acquire to short-circuit duplicate fetch" << dendl;

  // A peer (e.g., the backfill daemon completing while we were taking the
  // lock) may have populated the object.  If so we skip the S3 round-trip
  // and re-issue the RADOS read instead.
  using klass = ObjectReadRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_recheck_oid_after_lock>(this);

  int r = image_ctx->data_ctx.aio_stat(this->m_oid, rados_completion, nullptr, nullptr);
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_recheck_oid_after_lock(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  if (r == -ENOENT) {
    ldout(cct, 15) << "object still missing after lock, proceeding to S3" << dendl;
    read_from_s3();
    return;
  }
  if (r < 0) {
    ldout(cct, 5) << "stat failed after lock: " << cpp_strerror(r)
                  << ", proceeding to S3 anyway" << dendl;
    read_from_s3();
    return;
  }

  // Object now exists in RADOS (peer populated it).  Release lock and
  // re-issue the RADOS read.
  ldout(cct, 10) << "object now populated; releasing lock and re-reading from RADOS"
                 << dendl;
  unlock_after_s3_read();
  read_object();
}

template <typename I>
void ObjectReadRequest<I>::unlock_after_s3_read() {
  if (!m_s3_lock_acquired) {
    return;
  }
  // Invariant: m_s3_lock_acquired and m_skip_writeback are mutually
  // exclusive.  If both were set we'd unlock a lock the request never
  // actually holds — m_lock_cookie is populated during the lock attempt
  // even on the loser path, so the cls op would silently break the real
  // holder's lease.  Crash here rather than corrupt the holder's state.
  ceph_assert(!m_skip_writeback);
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 15) << "unlocking " << m_lock_oid << dendl;

  // Fire-and-forget unlock — failures are tolerable since cls_lock auto-expires
  // via duration.  No callback to 'this': the request may be deleted before
  // the unlock completes.
  librados::ObjectWriteOperation unlock_op;
  rados::cls::lock::unlock(&unlock_op, S3_FETCH_LOCK_NAME, m_lock_cookie);

  auto rados_completion = librados::Rados::aio_create_completion();
  int r = image_ctx->data_ctx.aio_operate(m_lock_oid, rados_completion, &unlock_op);
  rados_completion->release();
  if (r < 0) {
    ldout(cct, 5) << "warning: failed to submit async unlock: "
                  << cpp_strerror(r) << dendl;
  }
  m_s3_lock_acquired = false;
}

namespace {
// Skip-writeback peer-writeback wait tunables.  Hard-coded for now; if the
// values need to change for a workload, exposing them as rbd_* config
// options is a one-line addition.  The defaults give a worst-case 250 ms
// extra latency on the loser's first read, which is shorter than the S3
// fetch they just performed and shorter than the user-visible read RTT
// against any remote S3.  The peer's write_full of a 4 MB object on local
// RADOS lands in 10-50 ms; on cloud RADOS, 50-200 ms.
constexpr uint32_t PEER_WRITEBACK_MAX_POLLS = 5;
constexpr uint32_t PEER_WRITEBACK_POLL_INTERVAL_MS = 50;
} // anonymous namespace

template <typename I>
void ObjectReadRequest<I>::wait_for_peer_writeback() {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  // m_read_data already holds the bytes we'll return to the caller.  This
  // poll only exists to keep RADOS warm for subsequent in-process reads:
  // every iteration is a non-mutating aio_stat, costing one RADOS RTT.
  using klass = ObjectReadRequest<I>;
  librados::AioCompletion *c = util::create_rados_callback<
    klass, &klass::handle_peer_writeback_poll>(this);

  m_peer_poll_count++;
  ldout(cct, 20) << "peer-writeback poll #" << m_peer_poll_count
                 << " on " << this->m_oid << dendl;

  int r = image_ctx->data_ctx.aio_stat(this->m_oid, c, nullptr, nullptr);
  ceph_assert(r == 0);
  c->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_peer_writeback_poll(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  if (r == 0) {
    // Peer's writeback landed.  Subsequent reads will hit RADOS cache.
    ldout(cct, 15) << "peer writeback landed after "
                   << m_peer_poll_count << " poll(s); finishing" << dendl;
    this->finish(0);
    return;
  }

  if (m_peer_poll_count >= PEER_WRITEBACK_MAX_POLLS) {
    // Wait budget exhausted (~250 ms by default).  Return data to the
    // caller anyway — the peer is unusually slow, and worst case is the
    // next read also fetches from S3.  No correctness impact; the in-memory
    // bytes we hold are valid regardless of whether RADOS has a copy yet.
    ldout(cct, 10) << "peer writeback didn't land within "
                   << (PEER_WRITEBACK_POLL_INTERVAL_MS * PEER_WRITEBACK_MAX_POLLS)
                   << "ms; finishing without waiting further" << dendl;
    this->finish(0);
    return;
  }

  // ENOENT (or any other non-zero r) — schedule the next poll after a short
  // delay.  Use the ImageCtx-level SafeTimer so we don't block any of the
  // current callback's threads.  Pattern mirrors CopyupRequest's
  // retry_read_from_parent timer wiring.
  // Capture m_cancelled by value (shared_ptr): if `this` gets destroyed
  // before the timer fires, the flag persists and the lambda bails before
  // dereferencing freed memory.  See ~ObjectReadRequest() for the store.
  SafeTimer *timer;
  Mutex *timer_lock;
  ImageCtx::get_timer_instance(cct, &timer, &timer_lock);
  auto cancelled = m_cancelled;
  Mutex::Locker locker(*timer_lock);
  timer->add_event_after(
    PEER_WRITEBACK_POLL_INTERVAL_MS / 1000.0,
    new FunctionContext([this, cancelled](int) {
      if (cancelled->load()) return;
      wait_for_peer_writeback();
    }));
}

template <typename I>
void ObjectReadRequest<I>::read_from_s3() {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  ldout(cct, 10) << "fetching object " << this->m_object_no << " from S3" << dendl;

  // Capture everything needed from image_ctx while holding snap_lock (read).
  S3Config s3_config;
  uint64_t byte_start;
  uint64_t byte_length;
  std::string s3_url;

  {
    RWLock::RLocker snap_locker(image_ctx->snap_lock);

    if (!image_ctx->s3_config.is_valid()) {
      lderr(cct) << "invalid S3 configuration" << dendl;
      this->finish(-EINVAL);
      return;
    }

    // Copy config before releasing the lock.
    s3_config = image_ctx->s3_config;

    // For non-sparse raw images in S3, fetch the ENTIRE object: the full
    // data is written back to RADOS so subsequent reads of any offset hit
    // the cache rather than S3.
    uint64_t object_size = image_ctx->get_object_size();
    byte_start = this->m_object_no * object_size;
    byte_length = object_size;

    s3_url = s3_config.build_url();

    ldout(cct, 10) << "S3 URL: " << s3_url
                   << ", fetching entire object " << this->m_object_no
                   << " range: bytes=" << byte_start
                   << "-" << (byte_start + byte_length - 1) << dendl;

    // Fast-path: grab the already-initialized shared fetcher.
    if (image_ctx->s3_fetcher) {
      m_s3_fetcher = image_ctx->s3_fetcher;
    }
  }

  // Slow-path (first S3 read on this image): initialize the shared fetcher
  // under write lock so concurrent reads share the curl connection pool.
  if (!m_s3_fetcher) {
    RWLock::WLocker snap_wlocker(image_ctx->snap_lock);
    if (!image_ctx->s3_fetcher) {
      image_ctx->s3_fetcher =
        std::make_shared<io::S3ObjectFetcher>(cct, s3_config, image_ctx->get_object_size());
    }
    m_s3_fetcher = image_ctx->s3_fetcher;
  }

  using klass = ObjectReadRequest<I>;
  Context *ctx = util::create_context_callback<klass, &klass::handle_read_from_s3>(this);

  m_s3_fetcher->fetch_url(s3_url, m_read_data, ctx, byte_start, byte_length);
}

template <typename I>
void ObjectReadRequest<I>::handle_read_from_s3(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 10) << "S3 fetch result: r=" << r
                             << " bytes=" << m_read_data->length() << dendl;

  if (image_ctx->perfcounter) {
    if (r < 0) {
      image_ctx->perfcounter->inc(l_librbd_s3_fetch_errors);
    } else {
      image_ctx->perfcounter->inc(l_librbd_s3_fetch_count);
      image_ctx->perfcounter->inc(l_librbd_s3_fetch_bytes, m_read_data->length());
    }
  }

  if (r < 0) {
    // For sparse images, objects may not exist in S3
    if (r == -ENOENT || r == -EINVAL) {
      ldout(image_ctx->cct, 10) << "object " << this->m_object_no
                                 << " does not exist in S3 (sparse image)" << dendl;
      unlock_after_s3_read();
      this->finish(-ENOENT);
      return;
    }

    lderr(image_ctx->cct) << "failed to fetch from S3: " << cpp_strerror(r) << dendl;

    // Skip-writeback path: the peer foreground user holds the s3_fetch_lock
    // and is committed to populating m_oid in RADOS via its own write_full.
    // Our own S3 fetch failed (network blip, throttling, transient 5xx, etc.)
    // but the peer may have completed its write_full while we were failing.
    // Attempt one RADOS read before propagating the original S3 error.
    //
    // Safety / boundedness:
    //   * we never acquired the s3_fetch_lock on this path
    //     (m_s3_lock_acquired is false), so we deliberately do NOT call
    //     unlock_after_s3_read() — that would assert / break the peer's
    //     lock if cookies happen to match;
    //   * handle_skip_writeback_fallback is a leaf: it ALWAYS calls
    //     this->finish() and never dispatches further work — no
    //     re-entry into read_object()/should_read_from_s3()/
    //     read_from_s3_with_lock(), so there is zero risk of a recursive
    //     S3 fetch loop;
    //   * cost is exactly one extra RADOS RTT in the failure case.
    if (m_skip_writeback) {
      ldout(image_ctx->cct, 10)
          << "skip-writeback S3 fetch failed; attempting one RADOS fallback "
          << "read of " << this->m_oid << " before propagating error" << dendl;
      m_skip_writeback_fallback_err = r;

      m_read_data->clear();
      librados::ObjectReadOperation op;
      op.read(this->m_object_off, this->m_object_len, m_read_data, nullptr);
      // Carry the caller's op flags (matches read_object's primary path).
      op.set_op_flags2(m_op_flags);

      using klass = ObjectReadRequest<I>;
      librados::AioCompletion *rados_completion =
          util::create_rados_callback<
              klass, &klass::handle_skip_writeback_fallback>(this);
      int flags = image_ctx->get_read_flags(this->m_snap_id);
      int rr = image_ctx->data_ctx.aio_operate(
          this->m_oid, rados_completion, &op, flags, nullptr,
          (this->m_trace.valid() ? this->m_trace.get_info() : nullptr));
      ceph_assert(rr == 0);
      rados_completion->release();
      return;
    }

    unlock_after_s3_read();
    this->finish(r);
    return;
  }

  // Successfully fetched entire object from S3
  ldout(image_ctx->cct, 10) << "successfully fetched " << m_read_data->length()
                             << " bytes (entire object) from S3" << dendl;

  // Extract only the requested range for the read result
  // The full object will be written back to RADOS cache
  bufferlist full_object_data;
  full_object_data.claim(*m_read_data);  // Take ownership of full data

  // The S3 response may be shorter than object_size (e.g., the last object of
  // an image whose size isn't an exact multiple of object_size).  Zero-pad so
  // that substr_of never reads past the end.
  uint64_t needed = this->m_object_off + this->m_object_len;
  if (full_object_data.length() < needed) {
    ldout(image_ctx->cct, 10) << "S3 returned " << full_object_data.length()
                               << " bytes, zero-padding to " << needed << dendl;
    full_object_data.append_zero(needed - full_object_data.length());
  }

  // Extract requested range
  m_read_data->substr_of(full_object_data, this->m_object_off, this->m_object_len);

  ldout(image_ctx->cct, 10) << "extracted requested range: offset=" << this->m_object_off
                             << " len=" << this->m_object_len
                             << " from full object (" << full_object_data.length()
                             << " bytes)" << dendl;

  // Write-back the full object to RADOS, then complete the read.  We WAIT
  // for the write to land before completing the user's read so that any
  // immediately-subsequent read of the same object hits the local cache
  // instead of re-fetching from S3.  This eliminates the
  //   read 1: fetch S3 → fire-and-forget write-back → return
  //   read 2: parent ENOENT (write-back not landed yet) → fetch S3 again
  // race that doubles S3 traffic for sequential reads.  The latency cost
  // is one extra RADOS RTT per fetch (typically a few ms) — well under
  // the cost of a duplicate S3 GET (typically 50–200 ms over WAN).
  write_back_s3_data_then_finish(full_object_data);
}

template <typename I>
void ObjectReadRequest<I>::handle_skip_writeback_fallback(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  if (r >= 0) {
    // The peer's write_full landed in RADOS while our own S3 fetch was
    // failing.  m_read_data now holds the requested range from local
    // cache — return success and bury the original S3 error.
    ldout(cct, 10)
        << "skip-writeback fallback: RADOS read served " << r
        << " bytes from " << this->m_oid << " (peer writeback landed); "
        << "ignoring original S3 error "
        << cpp_strerror(m_skip_writeback_fallback_err) << dendl;
    this->finish(0);
    return;
  }

  // RADOS also missed (typically -ENOENT — peer's write_full hadn't
  // landed yet, or itself failed).  Surface the original S3 error to
  // the caller; the RADOS error is just diagnostic.
  ldout(cct, 5)
      << "skip-writeback fallback: RADOS read of " << this->m_oid
      << " failed with " << cpp_strerror(r)
      << "; propagating original S3 error "
      << cpp_strerror(m_skip_writeback_fallback_err) << dendl;
  this->finish(m_skip_writeback_fallback_err);
}

template <typename I>
void ObjectReadRequest<I>::write_back_s3_data_then_finish(
    bufferlist& full_object_data) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  // Cross-process dedup: when another foreground user holds the s3_fetch_lock,
  // we set m_skip_writeback = true and fetched our own copy in parallel.  The
  // lock holder is responsible for the write_full + object_map update; us
  // doing it too would just waste RADOS IOPS.  Note: we never acquired the
  // lock in this case, so unlock_after_s3_read() is a no-op.
  //
  // Don't call finish() yet — wait briefly for the peer's writeback to land
  // in RADOS so subsequent in-process reads of this object hit local cache
  // instead of refetching.  See wait_for_peer_writeback().
  if (m_skip_writeback) {
    ldout(cct, 10) << "skipping writeback: peer user holds lock, will populate"
                   << dendl;
    wait_for_peer_writeback();
    return;
  }

  // Zero-block sparseness: if S3 returned all zeros, skip both the data
  // write-back and the object_map update so the parent pool stays sparse.
  // Same trade-off as ObjectBackfillRequest::write_rados — we re-fetch on
  // the next read rather than persisting a 4 MB object full of zeros.
  if (full_object_data.is_zero()) {
    ldout(cct, 10) << "object " << this->m_oid
                   << " is all-zero from S3, skipping write-back + map update" << dendl;
    unlock_after_s3_read();
    this->finish(0);
    return;
  }

  ldout(cct, 10) << "writing " << full_object_data.length()
                 << " bytes (entire object) to parent cache object: "
                 << this->m_oid << dendl;

  // write_full() replaces rather than offsets — correct since we fetched the whole object
  librados::ObjectWriteOperation write_op;
  write_op.write_full(full_object_data);

  // Completion: fire object_map update (still fire-and-forget, since the
  // map is advisory) then complete the user's read.  We chain via a
  // FunctionContext rather than a member-pointer callback because the
  // RADOS completion callback signature does not match Context's.
  using klass = ObjectReadRequest<I>;
  auto on_writeback_done = util::create_context_callback<
      klass, &klass::handle_write_back_done>(this);

  auto rados_completion = util::create_rados_callback(on_writeback_done);

  int r = image_ctx->data_ctx.aio_operate(this->m_oid, rados_completion, &write_op);
  if (r < 0) {
    ldout(cct, 5) << "warning: failed to submit S3 cache write-back: "
                  << cpp_strerror(r) << dendl;
    rados_completion->release();
    // Read itself succeeded; surface success to the user even if cache
    // write-back failed.  The next reader will re-fetch from S3.
    unlock_after_s3_read();
    this->finish(0);
    return;
  }
  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_write_back_done(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  if (r < 0) {
    ldout(cct, 5) << "warning: S3 cache write-back failed: "
                  << cpp_strerror(r) << "; continuing" << dendl;
  } else {
    ldout(cct, 10) << "S3 cache write-back persisted for " << this->m_oid << dendl;
    // Object map update is fire-and-forget — its only purpose is to keep
    // rbd-du honest, and missing it does not affect read correctness.
    update_object_map_for_s3_write_back();
  }

  // Release the s3_fetch_lock so peers waiting to acquire it (or to break
  // a stale lock) can proceed.  Fire-and-forget: this request will finish
  // before the unlock RADOS round-trip completes, which is fine — cls_lock
  // auto-expires via the configured TTL if the unlock is lost.
  unlock_after_s3_read();
  this->finish(0);
}

template <typename I>
void ObjectReadRequest<I>::update_object_map_for_s3_write_back() {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;

  bool has_object_map = (image_ctx->features & RBD_FEATURE_OBJECT_MAP) != 0;

  if (!has_object_map) {
    ldout(cct, 15) << "image does not have object_map feature, skipping" << dendl;
    return;
  }

  if (image_ctx->object_map != nullptr) {
    // In-memory object map is available - update it synchronously (fast)
    RWLock::WLocker object_map_locker(image_ctx->object_map_lock);

    uint8_t new_state = OBJECT_EXISTS;
    auto obj_map = image_ctx->object_map;

    if ((*obj_map)[this->m_object_no] != new_state) {
      (*obj_map)[this->m_object_no] = new_state;

      ldout(cct, 10) << "updated in-memory object map for object "
                     << this->m_object_no << " to OBJECT_EXISTS" << dendl;

      Context *ctx = new FunctionContext([](int r) {});

      ZTracer::Trace trace;
      bool sent = obj_map->template aio_update<Context>(
        CEPH_NOSNAP, this->m_object_no, new_state, boost::optional<uint8_t>(),
        trace, false, ctx);

      if (!sent) {
        delete ctx;
        ldout(cct, 10) << "object map update not queued (no exclusive lock)" << dendl;
      }
    }
  } else {
    // object_map == nullptr: no exclusive lock held.  Use the same
    // fire-and-forget RADOS cls approach as CopyupRequest::fire_parent_s3_writeback().
    // Setting OBJECT_EXISTS is idempotent; we have just written the RADOS
    // object so the map must reflect it.  Skipping and deferring to "the next
    // exclusive-lock holder" is wrong for S3-backed parents that may never
    // acquire an exclusive lock, causing rbd-du to permanently show 0.
    const std::string object_map_name =
        ObjectMap<I>::object_map_name(image_ctx->id, CEPH_NOSNAP);
    librados::ObjectWriteOperation map_op;
    ObjectMap<I>::build_update_op(&map_op, this->m_object_no,
                                  this->m_object_no + 1,
                                  OBJECT_EXISTS,
                                  boost::optional<uint8_t>());
    auto c = librados::Rados::aio_create_completion();
    image_ctx->data_ctx.aio_operate(object_map_name, c, &map_op);
    c->release();
    ldout(cct, 10) << "fired RADOS cls object map update for object "
                   << this->m_object_no << " (no exclusive lock)" << dendl;
  }
}

/** write **/

template <typename I>
AbstractObjectWriteRequest<I>::AbstractObjectWriteRequest(
    I *ictx, const std::string &oid, uint64_t object_no, uint64_t object_off,
    uint64_t len, const ::SnapContext &snapc, const char *trace_name,
    const ZTracer::Trace &parent_trace, Context *completion)
  : ObjectRequest<I>(ictx, oid, object_no, object_off, len, CEPH_NOSNAP,
                     trace_name, parent_trace, completion),
    m_snap_seq(snapc.seq.val)
{
  m_snaps.insert(m_snaps.end(), snapc.snaps.begin(), snapc.snaps.end());

  if (this->m_object_off == 0 &&
      this->m_object_len == ictx->get_object_size()) {
    m_full_object = true;
  }

  compute_parent_info();

  ictx->snap_lock.get_read();
  if (!ictx->migration_info.empty()) {
    m_guarding_migration_write = true;
  }
  ictx->snap_lock.put_read();
}

template <typename I>
void AbstractObjectWriteRequest<I>::compute_parent_info() {
  I *image_ctx = this->m_ictx;
  RWLock::RLocker snap_locker(image_ctx->snap_lock);
  RWLock::RLocker parent_locker(image_ctx->parent_lock);

  this->compute_parent_extents(&m_parent_extents, false);

  if (!this->has_parent() ||
      (m_full_object && m_snaps.empty() && !is_post_copyup_write_required())) {
    m_copyup_enabled = false;
  }
}

template <typename I>
void AbstractObjectWriteRequest<I>::add_write_hint(
    librados::ObjectWriteOperation *wr) {
  I *image_ctx = this->m_ictx;
  RWLock::RLocker snap_locker(image_ctx->snap_lock);
  if (image_ctx->object_map == nullptr || !this->m_object_may_exist) {
    ObjectRequest<I>::add_write_hint(*image_ctx, wr);
  }
}

template <typename I>
void AbstractObjectWriteRequest<I>::send() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << this->get_op_type() << " " << this->m_oid << " "
                            << this->m_object_off << "~" << this->m_object_len
                            << dendl;
  {
    RWLock::RLocker snap_lock(image_ctx->snap_lock);
    if (image_ctx->object_map == nullptr) {
      m_object_may_exist = true;
    } else {
      // should have been flushed prior to releasing lock
      ceph_assert(image_ctx->exclusive_lock->is_lock_owner());
      m_object_may_exist = image_ctx->object_map->object_may_exist(
        this->m_object_no);
    }
  }

  if (!m_object_may_exist && is_no_op_for_nonexistent_object()) {
    ldout(image_ctx->cct, 20) << "skipping no-op on nonexistent object"
                              << dendl;
    this->async_finish(0);
    return;
  }

  pre_write_object_map_update();
}

template <typename I>
void AbstractObjectWriteRequest<I>::pre_write_object_map_update() {
  I *image_ctx = this->m_ictx;

  image_ctx->snap_lock.get_read();
  if (image_ctx->object_map == nullptr || !is_object_map_update_enabled()) {
    image_ctx->snap_lock.put_read();
    write_object();
    return;
  }

  if (!m_object_may_exist && m_copyup_enabled) {
    // optimization: copyup required
    image_ctx->snap_lock.put_read();
    copyup();
    return;
  }

  uint8_t new_state = this->get_pre_write_object_map_state();
  ldout(image_ctx->cct, 20) << this->m_oid << " " << this->m_object_off
                            << "~" << this->m_object_len << dendl;

  image_ctx->object_map_lock.get_write();
  if (image_ctx->object_map->template aio_update<
        AbstractObjectWriteRequest<I>,
        &AbstractObjectWriteRequest<I>::handle_pre_write_object_map_update>(
          CEPH_NOSNAP, this->m_object_no, new_state, {}, this->m_trace, false,
          this)) {
    image_ctx->object_map_lock.put_write();
    image_ctx->snap_lock.put_read();
    return;
  }

  image_ctx->object_map_lock.put_write();
  image_ctx->snap_lock.put_read();
  write_object();
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_pre_write_object_map_update(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;
  if (r < 0) {
    lderr(image_ctx->cct) << "failed to update object map: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  write_object();
}

template <typename I>
void AbstractObjectWriteRequest<I>::write_object() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  librados::ObjectWriteOperation write;
  if (m_copyup_enabled) {
    ldout(image_ctx->cct, 20) << "guarding write" << dendl;
    if (m_guarding_migration_write) {
      cls_client::assert_snapc_seq(
        &write, m_snap_seq, cls::rbd::ASSERT_SNAPC_SEQ_LE_SNAPSET_SEQ);
    } else {
      write.assert_exists();
    }
  }

  add_write_hint(&write);
  add_write_ops(&write);
  ceph_assert(write.size() != 0);

  librados::AioCompletion *rados_completion = util::create_rados_callback<
    AbstractObjectWriteRequest<I>,
    &AbstractObjectWriteRequest<I>::handle_write_object>(this);
  int r = image_ctx->data_ctx.aio_operate(
    this->m_oid, rados_completion, &write, m_snap_seq, m_snaps,
    (this->m_trace.valid() ? this->m_trace.get_info() : nullptr));
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_write_object(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  r = filter_write_result(r);
  if (r == -ENOENT) {
    if (m_copyup_enabled) {
      copyup();
      return;
    }
  } else if (r == -ERANGE && m_guarding_migration_write) {
    image_ctx->snap_lock.get_read();
    m_guarding_migration_write = !image_ctx->migration_info.empty();
    image_ctx->snap_lock.put_read();

    if (m_guarding_migration_write) {
      copyup();
    } else {
      ldout(image_ctx->cct, 10) << "migration parent gone, restart io" << dendl;
      compute_parent_info();
      write_object();
    }
    return;
  } else if (r == -EILSEQ) {
    ldout(image_ctx->cct, 10) << "failed to write object" << dendl;
    this->finish(r);
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to write object: " << cpp_strerror(r)
                          << dendl;
    this->finish(r);
    return;
  }

  post_write_object_map_update();
}

template <typename I>
void AbstractObjectWriteRequest<I>::copyup() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  ceph_assert(!m_copyup_in_progress);
  m_copyup_in_progress = true;

  image_ctx->copyup_list_lock.Lock();
  auto it = image_ctx->copyup_list.find(this->m_object_no);
  if (it == image_ctx->copyup_list.end()) {
    auto new_req = CopyupRequest<I>::create(
      image_ctx, this->m_oid, this->m_object_no,
      std::move(this->m_parent_extents), this->m_trace);
    this->m_parent_extents.clear();

    // make sure to wait on this CopyupRequest
    new_req->append_request(this);
    image_ctx->copyup_list[this->m_object_no] = new_req;

    image_ctx->copyup_list_lock.Unlock();
    new_req->send();
  } else {
    it->second->append_request(this);
    image_ctx->copyup_list_lock.Unlock();
  }
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_copyup(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  ceph_assert(m_copyup_in_progress);
  m_copyup_in_progress = false;

  if (r < 0 && r != -ERESTART) {
    lderr(image_ctx->cct) << "failed to copyup object: " << cpp_strerror(r)
                          << dendl;
    this->finish(r);
    return;
  }

  if (r == -ERESTART || is_post_copyup_write_required()) {
    write_object();
    return;
  }

  post_write_object_map_update();
}

template <typename I>
void AbstractObjectWriteRequest<I>::post_write_object_map_update() {
  I *image_ctx = this->m_ictx;

  image_ctx->snap_lock.get_read();
  if (image_ctx->object_map == nullptr || !is_object_map_update_enabled() ||
      !is_non_existent_post_write_object_map_state()) {
    image_ctx->snap_lock.put_read();
    this->finish(0);
    return;
  }

  ldout(image_ctx->cct, 20) << dendl;

  // should have been flushed prior to releasing lock
  ceph_assert(image_ctx->exclusive_lock->is_lock_owner());
  image_ctx->object_map_lock.get_write();
  if (image_ctx->object_map->template aio_update<
        AbstractObjectWriteRequest<I>,
        &AbstractObjectWriteRequest<I>::handle_post_write_object_map_update>(
          CEPH_NOSNAP, this->m_object_no, OBJECT_NONEXISTENT, OBJECT_PENDING,
          this->m_trace, false, this)) {
    image_ctx->object_map_lock.put_write();
    image_ctx->snap_lock.put_read();
    return;
  }

  image_ctx->object_map_lock.put_write();
  image_ctx->snap_lock.put_read();
  this->finish(0);
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_post_write_object_map_update(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;
  if (r < 0) {
    lderr(image_ctx->cct) << "failed to update object map: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  this->finish(0);
}

template <typename I>
void ObjectWriteRequest<I>::add_write_ops(librados::ObjectWriteOperation *wr) {
  if (this->m_full_object) {
    wr->write_full(m_write_data);
  } else {
    wr->write(this->m_object_off, m_write_data);
  }
  wr->set_op_flags2(m_op_flags);
}

template <typename I>
void ObjectWriteSameRequest<I>::add_write_ops(
    librados::ObjectWriteOperation *wr) {
  wr->writesame(this->m_object_off, this->m_object_len, m_write_data);
  wr->set_op_flags2(m_op_flags);
}

template <typename I>
void ObjectCompareAndWriteRequest<I>::add_write_ops(
    librados::ObjectWriteOperation *wr) {
  wr->cmpext(this->m_object_off, m_cmp_bl, nullptr);

  if (this->m_full_object) {
    wr->write_full(m_write_bl);
  } else {
    wr->write(this->m_object_off, m_write_bl);
  }
  wr->set_op_flags2(m_op_flags);
}

template <typename I>
int ObjectCompareAndWriteRequest<I>::filter_write_result(int r) const {
  if (r <= -MAX_ERRNO) {
    I *image_ctx = this->m_ictx;
    Extents image_extents;

    // object extent compare mismatch
    uint64_t offset = -MAX_ERRNO - r;
    Striper::extent_to_file(image_ctx->cct, &image_ctx->layout,
                            this->m_object_no, offset, this->m_object_len,
                            image_extents);
    ceph_assert(image_extents.size() == 1);

    if (m_mismatch_offset) {
      *m_mismatch_offset = image_extents[0].first;
    }
    r = -EILSEQ;
  }
  return r;
}

} // namespace io
} // namespace librbd

template class librbd::io::ObjectRequest<librbd::ImageCtx>;
template class librbd::io::ObjectReadRequest<librbd::ImageCtx>;
template class librbd::io::AbstractObjectWriteRequest<librbd::ImageCtx>;
template class librbd::io::ObjectWriteRequest<librbd::ImageCtx>;
template class librbd::io::ObjectDiscardRequest<librbd::ImageCtx>;
template class librbd::io::ObjectWriteSameRequest<librbd::ImageCtx>;
template class librbd::io::ObjectCompareAndWriteRequest<librbd::ImageCtx>;
