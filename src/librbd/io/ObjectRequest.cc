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
#include "librbd/io/AsyncOperation.h"
#include "librbd/io/AsyncWritebackThrottler.h"
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
        !image_ctx->object_map->object_may_exist(this->m_object_no) &&
        // Trust the object_map (and short-circuit on NONEXISTENT) only
        // when EITHER:
        //   (a) the image isn't S3-backed at all (regular clone / plain
        //       image -- existing behaviour), or
        //   (b) the image IS S3-backed and rbd-backfill has reported
        //       backfill_status=complete, meaning the object_map is the
        //       authoritative source: NONEXISTENT = known-zero, return
        //       -ENOENT and let the child layer zero-fill (standard
        //       sparse-clone semantics).
        //
        // Without this gate, a stale-bit can drive an infinite refetch
        // loop: handle_read_from_s3 sets the in-memory bit to OBJECT_
        // EXISTS optimistically (before the throttler confirms), the
        // throttler drops over-cap, RADOS doesn't get the object,
        // subsequent reads see the bit say EXISTS -> aio_operate ->
        // ENOENT -> should_read_from_s3 -> S3 fetch -> drop again ->
        // repeat.  After backfill complete we trust the bit and
        // short-circuit zero-blocks straight to ENOENT (no S3 fetch).
        //
        // For mid-backfill or never-backfilled S3-backed images we
        // fall through to aio_operate so genuinely-unfetched objects
        // can still be served via the S3 fallback path.
        (!image_ctx->s3_config.is_valid() ||
         image_ctx->s3_backfill_complete)) {
      image_ctx->op_work_queue->queue(new FunctionContext([this](int r) {
          read_parent();
        }), 0);
      return;
    }
  }

  // Pre-aio short-circuit using the per-image backfill-visited bitmap.
  // The object_map check above is unreachable for read-only parent opens
  // (RefreshRequest::send_v2_open_object_map only loads object_map when
  // exclusive_lock is held or the open is a snapshot read), so without
  // this block every zero-block parent read pays a RADOS RTT to discover
  // ENOENT before handle_read_object's ENOENT branch can trust the
  // bitmap.  Trusting it here saves that RTT, bringing zero-block read
  // latency on an S3-backed parent into parity with a regular RBD clone
  // (whose object_map IS loaded for snap reads).
  //
  // Safety: this branch's design treats the S3 source as immutable --
  // the parent's RADOS pool is purely a cache.  A bitmap bit marked
  // VISITED_ZERO means the daemon observed S3[N] = zero; subsequent
  // S3 mutation is not a supported in-place op (re-backfill required),
  // so the bit cannot become stale relative to reality.  Note in
  // particular that no code path can populate parent RADOS for a
  // VISITED_ZERO object: CopyupRequest::fire_parent_s3_writeback
  // returns early when m_copyup_data.length() == 0; AsyncWritebackThrottler::
  // write_full short-circuits on is_zero(); handle_read_from_s3 skips
  // the throttler when the fetched data is zero.  Under that invariant
  // pre-aio finishing with -ENOENT is correct: the upper layer zero-
  // fills, equivalent to what aio_operate -> ENOENT -> handle_read_object's
  // bitmap-trust branch would have produced after one extra RADOS RTT.
  //
  // Finish with -ENOENT directly (rather than calling read_parent()
  // like the object_map short-circuit above) because: (a) it mirrors
  // the post-aio bitmap-trust branch in handle_read_object, which also
  // calls finish(-ENOENT); (b) it skips the snap_lock + parent_lock +
  // Striper work read_parent performs only to land at the same
  // finish(-ENOENT) for an S3-backed parent (S3-backed parents are
  // always terminal in the clone chain in this design -- they have no
  // grandparent of their own, so read_parent's parent_overlap check
  // always falls through to -ENOENT).
  //
  // s3_config.is_valid() is read inside snap_lock to pair with the
  // documented "apply_metadata populates s3_config without snap_lock"
  // hazard (see ImageCtx.cc maybe_reload_backfill_visited).  Without
  // snap_lock a concurrent RefreshRequest mid-populating the S3Config's
  // std::string members could produce a torn is_valid() read.
  //
  // For VISITED_NO and VISITED_HAS_DATA we fall through to aio_operate
  // as before; handle_read_object's post-aio bitmap check still serves
  // as the partial-backfill fallback for objects the daemon hasn't
  // visited.
  if (image_ctx->s3_fetch_enabled && this->m_snap_id == CEPH_NOSNAP) {
    bool s3_valid;
    {
      RWLock::RLocker snap_locker(image_ctx->snap_lock);
      s3_valid = image_ctx->s3_config.is_valid();
    }
    if (s3_valid) {
      auto visited = std::atomic_load(&image_ctx->backfill_visited);
      if (visited && this->m_object_no < visited->size() &&
          (*visited)[this->m_object_no] == RBD_BACKFILL_VISITED_ZERO) {
        ldout(image_ctx->cct, 10) << "object " << this->m_object_no
                                  << " short-circuiting pre-aio via bitmap "
                                  << "(VISITED_ZERO)" << dendl;
        image_ctx->op_work_queue->queue(new FunctionContext([this](int r) {
            this->finish(-ENOENT);
          }), 0);
        return;
      }
      // In-memory known-zero cache (P1): same pre-aio RTT saving as the
      // bitmap branch above, but works even when no on-disk bitmap exists
      // (cold, never-backfilled parent).  Populated by handle_read_from_s3
      // on the first all-zero fetch of this object on this ImageCtx.
      if (image_ctx->is_known_zero_object(this->m_object_no)) {
        ldout(image_ctx->cct, 10) << "object " << this->m_object_no
                                  << " short-circuiting pre-aio via in-memory "
                                  << "known-zero cache" << dendl;
        image_ctx->op_work_queue->queue(new FunctionContext([this](int r) {
            this->finish(-ENOENT);
          }), 0);
        return;
      }
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
      // After backfill completes the parent's RADOS pool is authoritative:
      // any object NOT present in RADOS was deliberately skipped by
      // ObjectBackfillRequest::write_rados because is_zero() returned true.
      // Going to S3 for these is wasted work -- we KNOW the object is
      // all-zero.  Finish with -ENOENT and let ImageRequest::aio_read's
      // sparse-clone semantics zero-fill at the higher layer, matching
      // the regular-clone path's behaviour for "no parent at this offset".
      //
      // This is the post-backfill fast path that the read_object()
      // object_map short-circuit can't reach for read-only parent opens
      // (object_map is loaded only for snap or exclusive-lock owners --
      // see send_v2_open_object_map at RefreshRequest.cc:1062-1071), so
      // parent-via-read_parent chains were going aio_operate -> ENOENT
      // -> S3 fetch on every zero-block read, defeating backfill.
      // The s3_backfill_complete flag and the backfill-visited bitmap both
      // reflect HEAD state.  Snapshot reads must NOT consult them: a
      // snapshot that was taken before backfill visited an object captures
      // that object as it was at snap time, which may differ from the
      // post-backfill HEAD truth.  E.g. a snap holding non-zero S3 data
      // for object N, with HEAD bitmap saying VISITED_ZERO (the daemon
      // confirmed the CURRENT S3 has zero there), would wrongly return
      // zeros for the snap.  Let snapshot reads fall through to the
      // normal S3 path; per-snap trust would need per-snap bitmaps, which
      // we don't have today.
      if (this->m_snap_id == CEPH_NOSNAP) {
        if (image_ctx->s3_backfill_complete) {
          ldout(image_ctx->cct, 10) << "object " << this->m_object_no
                                    << " ENOENT after backfill complete; "
                                    << "skipping S3 fetch (known-zero)"
                                    << dendl;
          this->finish(-ENOENT);
          return;
        }

        // Per-object trust during PARTIAL backfill: even if the whole image
        // isn't yet `backfill_status=complete`, rbd-backfill may have
        // already visited THIS object and recorded its state in the
        // per-image bitmap (rbd_backfill_visited.<id>).  VISITED_ZERO
        // means the daemon confirmed the source S3 object is all-zero
        // and deliberately skipped writing it to RADOS -- going to S3
        // here would re-confirm "zero" at a 100+ ms RTT cost.
        //
        // Atomic shared_ptr load so a concurrent maybe_reload_backfill_visited()
        // swapping in a new bitmap can't free this snapshot under us.
        // Bounds-check guards against rbd resize after the bitmap was
        // sized: new objects past bitmap end fall through to S3.
        auto visited = std::atomic_load(&image_ctx->backfill_visited);
        if (visited && this->m_object_no < visited->size() &&
            (*visited)[this->m_object_no] == RBD_BACKFILL_VISITED_ZERO) {
          ldout(image_ctx->cct, 10) << "object " << this->m_object_no
                                    << " ENOENT with backfill_visited=ZERO; "
                                    << "skipping S3 fetch (per-object trust)"
                                    << dendl;
          this->finish(-ENOENT);
          return;
        }

        // Per-object trust from the in-memory known-zero cache (P1).  A prior
        // read on THIS ImageCtx fetched object_no from S3, found it all-zero,
        // and recorded it via ImageCtx::note_known_zero_object.  This covers
        // the cold, never-backfilled parent where no on-disk bitmap exists
        // (backfill_visited is null) -- without it, every read of a zero
        // object re-fetches 4 MB of zeros from S3.  Same -ENOENT zero-fill
        // semantics as the bitmap branch above.
        if (image_ctx->is_known_zero_object(this->m_object_no)) {
          ldout(image_ctx->cct, 10) << "object " << this->m_object_no
                                    << " ENOENT with in-memory known-zero; "
                                    << "skipping S3 fetch" << dendl;
          this->finish(-ENOENT);
          return;
        }

        // Bitmap miss (or no bitmap, or out-of-bounds): kick a non-blocking
        // background reload if our cached copy is older than
        // rbd_s3_backfill_visited_reload_interval.  This read still falls
        // through to S3; subsequent reads after the reload completes will
        // see any newly-flipped bits.  Critical for the concurrent-VM-boot
        // scenario where N ImageCtxs all snapshot the bitmap at open and
        // must pick up bits the daemon flips while they remain open.
        image_ctx->maybe_reload_backfill_visited();
      }
      // No cls_lock on the critical path — see the design comment in
      // ObjectRequest.h.  read_from_s3() directly issues the S3 GET; on
      // success, handle_read_from_s3() finishes the client read AND
      // submits a detached cache-populate writeback to
      // AsyncWritebackThrottler.  The natural RADOS-first read above
      // provides cross-process dedup: if a peer already wrote back,
      // our read_object() call would have found the bytes and never
      // reached this branch.
      read_from_s3();
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

  // Guard the callback against ImageCtx teardown: if the owning
  // ImageCtx is destroyed before the S3 worker fires our completion,
  // the cancel flag (captured by shared_ptr value) stays alive and
  // tells the worker's lambda to bail before dereferencing `this`,
  // which now points at freed memory.  Mirrors CopyupRequest.cc.
  auto cancelled = m_cancelled;
  auto ctx = new FunctionContext([this, cancelled](int r) {
    if (cancelled->load()) return;
    handle_read_from_s3(r);
  });

  m_s3_fetcher->fetch_url(s3_url, m_read_data, ctx, byte_start, byte_length,
                          m_cancelled, &m_fetched_from_cache);
}

template <typename I>
void ObjectReadRequest<I>::handle_read_from_s3(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  ldout(cct, 10) << "S3 fetch result: r=" << r
                 << " bytes=" << m_read_data->length() << dendl;

  if (image_ctx->perfcounter) {
    if (r < 0) {
      image_ctx->perfcounter->inc(l_librbd_s3_fetch_errors);
    } else {
      image_ctx->perfcounter->inc(l_librbd_s3_fetch_count);
      image_ctx->perfcounter->inc(l_librbd_s3_fetch_bytes,
                                  m_read_data->length());
    }
  }

  if (r < 0) {
    if (r == -ENOENT || r == -EINVAL) {
      ldout(cct, 10) << "object " << this->m_object_no
                     << " does not exist in S3 (sparse image)" << dendl;
      this->finish(-ENOENT);
      return;
    }
    // Transient S3 failure (5xx, network blip, TLS reset).  Try one
    // RADOS read of m_oid before surfacing EIO to the caller — a peer's
    // throttler-owned writeback may have populated the parent oid
    // during our failure window.  Leaf path: never re-enters S3.
    lderr(cct) << "failed to fetch from S3: " << cpp_strerror(r)
               << "; attempting one RADOS fallback read of " << this->m_oid
               << dendl;
    try_rados_fallback_on_s3_error(r);
    return;
  }

  ldout(cct, 10) << "successfully fetched " << m_read_data->length()
                 << " bytes (entire object) from S3" << dendl;

  // Take ownership of the full S3 payload before we slice the requested
  // sub-range into m_read_data.  full_object_data is what the throttler
  // takes (by move) for the detached cache-populate writeback.
  bufferlist full_object_data;
  full_object_data.claim(*m_read_data);

  // The S3 response may be shorter than object_size (last object of an
  // image whose size isn't an exact multiple).  Zero-pad so substr_of
  // never reads past the end.
  uint64_t needed = this->m_object_off + this->m_object_len;
  if (full_object_data.length() < needed) {
    ldout(cct, 10) << "S3 returned " << full_object_data.length()
                   << " bytes, zero-padding to " << needed << dendl;
    full_object_data.append_zero(needed - full_object_data.length());
  }

  // Slice out what the caller actually asked for (the read result).
  m_read_data->substr_of(full_object_data, this->m_object_off,
                         this->m_object_len);

  // ── Critical-path completes here ──────────────────────────────────────
  // The caller's read result is in m_read_data.  Submit the full-object
  // bytes to AsyncWritebackThrottler for detached cache populate
  // (cls_lock + write_full + obj_map update + unlock) and immediately
  // return to the caller.  The client does NOT wait on RADOS for the
  // cache populate — that work runs on the throttler's owned SM.
  //
  // Drop semantics: if the throttler is at its in-flight / bytes-in-flight
  // cap, try_submit returns false and the data is discarded.  Correctness
  // is preserved because the parent cache is opportunistic — the backfill
  // daemon's rescan or the next reader will populate it.  We log at debug
  // 10 so operators can see drops in the field without max-debug logging.
  //
  // Cross-process write dedup: the throttler's WritebackRequest will
  // EBUSY-drop on cls_lock acquire if a peer (another client, the
  // backfill daemon) is already mid-write.  Net RADOS write count is
  // bounded by the cls_lock the same way the old lock-on-critical-path
  // design bounded it — what changed is that the dance is post-finish().
  // LRU cache-hit short-circuit: an earlier ObjectReadRequest already
  // ran the in-memory object_map mutation + throttler submission +
  // readahead prefetch for this object_no.  Re-doing them now just
  // adds noise to the warm-cache invariants (RADOS write_ops counter
  // climbs via the throttler's EBUSY-drop attempts).  Bail to finish
  // with the data already in m_read_data.
  if (m_fetched_from_cache) {
    ldout(cct, 20) << "served from in-process LRU; skipping resubmit + "
                   << "prefetch" << dendl;
    this->finish(0);
    return;
  }

  // Update the in-memory object_map BEFORE submitting the throttler
  // writeback.  The pre-c2 ObjectReadRequest::update_object_map_for_s3
  // _write_back set both the on-disk bit (via cls) AND the in-memory
  // bit; c2 moved the on-disk update into the throttler's
  // WritebackRequest (which has no ImageCtx pointer and therefore
  // cannot touch the in-memory map) but inadvertently dropped the
  // in-memory mutation entirely.  Without it, rbd_du on a live ImageCtx
  // reports 0 used even after reads have populated the parent's RADOS
  // pool, and object_may_exist() returns false, mis-routing the next
  // read through read_parent() (no-op for standalone) → another S3
  // fetch.  Both regressions were caught by the C8 review finding.
  //
  // The on-disk update still runs inside the throttler's SM (via
  // ObjectMap<>::build_update_op against the parent's object_map_oid)
  // — matching the OLD code's "Path 2" — but now serialized through
  // the cls_lock.  This in-memory update is "Path 1" of the OLD design.
  //
  // !! SCOPE WARNING — DO NOT REUSE OUTSIDE STANDALONE-CLONE !!
  // The deleted ObjectMap::aio_update path that this raw write
  // replaces enforced two guards:
  //   (a) ExclusiveLock ownership: silently no-op if this client
  //       didn't hold the image's exclusive_lock, preventing a non-
  //       owner from mutating shared state that the actual owner is
  //       tracking authoritatively
  //   (b) bounds: aio_update would queue under proper sequencing
  // For the standalone-clone scope this is safe by construction:
  // (a) the image we mutate is the S3-BACKED PARENT, which is
  //     effectively read-only -- the only writes that ever land
  //     here are these opportunistic cache-populates issued by any
  //     reader, none of which need cross-client coordination
  //     because the data is identical (sourced from immutable S3)
  // (b) m_object_no is bounded by image size; spawn_prefetch past-EOF
  //     returns -ENOENT before reaching this point (line ~518)
  // If this code path is later reused for a WRITABLE parent (e.g.,
  // standalone clone of another standalone clone with copy-on-write
  // semantics layered in), reintroduce the aio_update ownership
  // check or this becomes a multi-writer object_map race.
  //
  // SKIP on all-zero data: WritebackRequest::write_full short-circuits
  // to release_lock on is_zero(), bypassing both the RADOS write_full
  // AND the on-disk object_map update.  If we set the in-memory bit to
  // OBJECT_EXISTS here, the in-mem and on-disk maps diverge: in-mem
  // says EXISTS, on-disk says NONEXISTENT, RADOS has no object.  Then
  // object_may_exist=true skips the parent-read shortcut, aio_read
  // returns ENOENT, and we refetch from S3 on every read for that
  // object -- defeats the warm-cache invariant for sparse parents.
  //
  // SKIP after backfill complete: ImageCtx::s3_backfill_complete signals
  // that rbd-backfill has already written every non-zero object to
  // RADOS and marked the on-disk object_map authoritatively.  Setting
  // the in-memory bit OPTIMISTICALLY here would create the same
  // stale-bit divergence as the zero-block case if the current
  // throttler submit happens to drop (over-cap, EBUSY, etc.): the bit
  // says EXISTS, RADOS doesn't have the object, every subsequent read
  // re-fetches from S3 in a loop.  After backfill complete the on-disk
  // map is the source of truth, so trusting it (and leaving the
  // in-memory bit alone for objects we did NOT genuinely populate) is
  // both more correct AND avoids the loop.
  // Compute once: is_zero() scans the whole ~4 MB buffer, and we branch on it
  // for the object_map update, the known-zero record, and the throttler skip.
  const bool full_is_zero = full_object_data.is_zero();

  if (image_ctx->object_map != nullptr &&
      !full_is_zero &&
      !image_ctx->s3_backfill_complete) {
    RWLock::RLocker owner_locker(image_ctx->owner_lock);
    if (image_ctx->object_map != nullptr) {
      RWLock::WLocker object_map_locker(image_ctx->object_map_lock);
      (*image_ctx->object_map)[this->m_object_no] = OBJECT_EXISTS;
    }
  }

  if (full_is_zero) {
    // All-zero S3 source (P1).  Record the object as known-zero so subsequent
    // reads short-circuit locally (read_object pre-aio + handle_read_object
    // ENOENT) instead of re-fetching 4 MB of zeros from S3 -- the dominant
    // waste on a cold, mostly-zero parent.
    //
    // Record ONLY for HEAD reads (m_snap_id == CEPH_NOSNAP).  Both consume
    // sites gate the known-zero short-circuit on CEPH_NOSNAP (read_object and
    // handle_read_object), for the reason spelled out at handle_read_object:
    // a snapshot may capture an object as it was at snap time, which can
    // differ from HEAD.  Recording a snapshot observation into the HEAD-
    // trusted known-zero set / VISITED_ZERO bitmap would let a snap read
    // poison HEAD (return zeros where HEAD has data) the moment per-snapshot
    // S3 content diverges from HEAD.  Today the S3 fetcher is snap-blind so
    // they agree, but gating here keeps record/consume symmetric and the
    // invariant robust.
    //
    // Trusting a recorded zero forever relies on the S3 source being
    // immutable (the project's contract: re-backfill required to change it).
    // Unlike the in-process LRU's 2 s TTL, this cache never expires, so a
    // mutated S3 object that became non-zero would not be re-fetched -- the
    // same exposure as the daemon-written VISITED_ZERO bitmap.
    if (this->m_snap_id == CEPH_NOSNAP) {
      image_ctx->note_known_zero_object(this->m_object_no);
    }
    // Skip the throttler entirely: AsyncWritebackThrottler's WritebackRequest
    // already short-circuits write_full on is_zero() (zeros are never written
    // to the parent RADOS pool), so submitting only burns a .s3lk cls_lock
    // acquire/release -- the EBUSY churn measured under concurrent boot.  Skip
    // readahead too: a zero object carries no warm-the-next-block signal, and
    // prefetching forward through a zero region would just S3-GET more zeros.
    // The caller's zero bytes are already sliced into m_read_data; finish.
    this->finish(0);
    return;
  }

  auto& throttler = AsyncWritebackThrottler::instance(cct);
  uint64_t bytes = full_object_data.length();
  bool submitted = throttler.try_submit(
      image_ctx->data_ctx, this->m_oid, this->m_object_no,
      std::move(full_object_data), image_ctx->id);
  if (!submitted) {
    ldout(cct, 10) << "throttler full; dropping cache populate for "
                   << this->m_oid << " (" << bytes << " bytes)" << dendl;
  }

  // Opportunistic readahead: spawn N more reads through the FULL pipeline
  // (read_object first, then read_from_s3 on RADOS miss, then
  // handle_read_from_s3 which submits the throttler).  Critical for
  // cold->warm correctness: bypassing the pipeline (calling
  // S3ObjectFetcher::prefetch_next directly) populates only the
  // per-process LRU, which evaporates on process exit -- the next
  // process re-fetches from S3 because the parent's RADOS pool was
  // never written.  Suppressed when m_is_prefetch=true so prefetch
  // chains don't compound (rbd_s3_readahead_objects=2 with depth=N
  // would fan out to 2^N reads otherwise).
  uint64_t readahead = cct->_conf.template get_val<uint64_t>(
      "rbd_s3_readahead_objects");
  if (readahead > 0 && !m_is_prefetch) {
    for (uint64_t i = 1; i <= readahead; i++) {
      ObjectReadRequest<I>::spawn_prefetch(image_ctx,
                                           this->m_object_no + i);
    }
  }

  this->finish(0);
}

template <typename I>
void ObjectReadRequest<I>::spawn_prefetch(I *ictx,
                                          uint64_t target_object_no) {
  // Allocate ALL resources first, registering the AsyncOperation LAST.
  // If we registered async_op->start_op on the parent xlist before
  // every other allocation succeeded, a subsequent `new` throwing
  // bad_alloc would leave the AsyncOperation permanently on the xlist
  // (its destructor asserts it is NOT on a list, but we never call
  // finish_op on the abandoned op).  ~ImageCtx -> wait_for_async_ops
  // would then deadlock waiting for an op that no one will complete.
  // Wrap the allocation block in try/catch so OOM is recoverable;
  // prefetch is opportunistic, so dropping it on bad_alloc is fine.
  ceph::bufferlist *buf = nullptr;
  ExtentMap *extent_map = nullptr;
  AsyncOperation *async_op = nullptr;
  FunctionContext *on_finish = nullptr;
  ObjectReadRequest<I> *req = nullptr;
  try {
    buf = new ceph::bufferlist();
    extent_map = new ExtentMap();
    async_op = new AsyncOperation();
    on_finish = new FunctionContext(
        [buf, extent_map, async_op](int r) {
          // Result ignored: prefetch is opportunistic.  Errors here
          // (past-EOF -ENOENT, S3 5xx) are not actionable; the
          // throttler submission + LRU populate + object_map update
          // either ran or didn't.  The next foreground read will
          // detect and recover.
          async_op->finish_op();
          delete async_op;
          delete buf;
          delete extent_map;
        });
    uint64_t object_size = ictx->get_object_size();
    std::string oid = ictx->get_object_name(target_object_no);
    req = ObjectReadRequest<I>::create(
        ictx, oid, target_object_no, /*offset=*/ 0, /*len=*/ object_size,
        CEPH_NOSNAP, /*op_flags=*/ 0, ZTracer::Trace{},
        buf, extent_map, on_finish);
    req->m_is_prefetch = true;
  } catch (...) {
    // Allocation failed before AsyncOperation was registered.  The
    // lambda's captures are still set but we delete the captured
    // objects manually here; the lambda itself never fires because
    // on_finish->complete() is never called.
    delete req;
    delete on_finish;
    delete async_op;
    delete extent_map;
    delete buf;
    return;
  }
  // All allocations succeeded.  Register the AsyncOperation only NOW,
  // so it is paired with a guaranteed-to-fire finish_op() via the
  // lambda above.
  async_op->start_op(*util::get_image_ctx(ictx));
  req->send();
}

template <typename I>
void ObjectReadRequest<I>::try_rados_fallback_on_s3_error(int s3_err) {
  I *image_ctx = this->m_ictx;
  m_s3_fallback_err = s3_err;

  // Single bounded RADOS read of m_oid.  If a peer's throttler-owned
  // writeback landed during our S3 failure window, this read returns
  // the data and we surface success to the caller.  If RADOS misses
  // too, handle_rados_fallback propagates the original S3 error.
  m_read_data->clear();
  librados::ObjectReadOperation op;
  op.read(this->m_object_off, this->m_object_len, m_read_data, nullptr);
  op.set_op_flags2(m_op_flags);

  using klass = ObjectReadRequest<I>;
  librados::AioCompletion *rados_completion =
    util::create_rados_callback<klass, &klass::handle_rados_fallback>(this);
  int flags = image_ctx->get_read_flags(this->m_snap_id);
  int r = image_ctx->data_ctx.aio_operate(
      this->m_oid, rados_completion, &op, flags, nullptr,
      (this->m_trace.valid() ? this->m_trace.get_info() : nullptr));
  ceph_assert(r == 0);
  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_rados_fallback(int r) {
  I *image_ctx = this->m_ictx;
  auto cct = image_ctx->cct;
  if (r >= 0) {
    ldout(cct, 10) << "RADOS fallback recovered " << m_read_data->length()
                   << " bytes after S3 fetch error "
                   << cpp_strerror(m_s3_fallback_err) << dendl;
    this->finish(0);
    return;
  }
  ldout(cct, 5) << "RADOS fallback also failed (" << cpp_strerror(r)
                << "); surfacing original S3 error "
                << cpp_strerror(m_s3_fallback_err) << dendl;
  this->finish(m_s3_fallback_err);
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
