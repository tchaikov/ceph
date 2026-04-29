// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ObjectBackfillRequest.h"
#include "BackfillDaemon.h"
#include "Types.h"
#include "librbd/Types.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "cls/lock/cls_lock_client.h"
#include "cls/rbd/cls_rbd_client.h"
#include "librbd/ObjectMap.h"
#include "include/stringify.h"
#include "librbd/Utils.h"
#include <boost/optional.hpp>

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
  const std::string& image_id,
  CephContext* cct,
  Context* on_finish)
  : m_parent_ioctx(parent_ioctx),  // Copy IoCtx - safe for async use
    m_parent_oid(parent_oid),
    m_object_no(object_no),
    m_image_id(image_id),
    m_cct(cct),
    m_on_finish(on_finish),
    m_lock("ObjectBackfillRequest::m_lock"),
    m_data_bl(data) {  // Store pre-fetched data

  // Generate a unique lock cookie: prefix + request address + wall-clock time.
  // Using the object address rather than pthread_self() avoids collisions from
  // thread-ID reuse after a daemon restart within the same second.
  m_lock_cookie = librbd::BACKFILL_LOCK_COOKIE_PREFIX +
                  stringify(reinterpret_cast<uintptr_t>(this)) + "-" +
                  stringify(ceph_clock_now());

  m_lock_name = librbd::S3_FETCH_LOCK_NAME;
  m_lock_tag = librbd::S3_FETCH_LOCK_TAG;  // Must match CopyupRequest's lock tag (librbd/Types.h)

  // Sentinel lock object: must match CopyupRequest::m_parent_lock_oid
  // (m_parent_oid + ".s3lk").  Using a separate sentinel prevents the
  // cls_lock side-effect of creating the data object as an empty RADOS
  // object, and ensures mutual exclusion with COW operations from child images.
  m_lock_oid = m_parent_oid + librbd::S3_FETCH_LOCK_SENTINEL_SUFFIX;
}

ObjectBackfillRequest::~ObjectBackfillRequest() {
}

void ObjectBackfillRequest::send() {
  dout(10) << "object_no=" << m_object_no << " oid=" << m_parent_oid << dendl;

  acquire_lock();
}

void ObjectBackfillRequest::acquire_lock() {
  dout(15) << dendl;

  Context* ctx = librbd::util::create_context_callback<ObjectBackfillRequest, &ObjectBackfillRequest::handle_acquire_lock>(this);

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

  int r = m_parent_ioctx.aio_operate(m_lock_oid, rados_completion, &op);
  ceph_assert(r == 0);
  rados_completion->release();
}

void ObjectBackfillRequest::handle_acquire_lock(int r) {
  dout(15) << "r=" << r << dendl;

  if (r < 0) {
    if (r == -EBUSY || r == -EEXIST) {
      // Lock already held by a CopyupRequest (client COW) that raced with the
      // daemon.  The COW operation is in progress and will write the parent
      // RADOS object before releasing the lock.  Treat this as success: the
      // object will be populated by the COW, so the daemon does not need to
      // write it.  Counting this as a failure would make the daemon report
      // -EIO for images that were correctly populated via COW.
      dout(5) << "lock busy on object " << m_object_no
              << ", client I/O has preempted daemon backfill — treating as success" << dendl;
      finish(0);
    } else {
      derr << "failed to acquire lock: " << cpp_strerror(r) << dendl;
      finish(r);
    }
    return;
  }

  dout(10) << "lock acquired for object " << m_object_no << dendl;

  write_rados();
}


void ObjectBackfillRequest::write_rados() {
  dout(15) << dendl;

  // Zero-block sparseness: if the S3 fetch returned all zeros, do not create
  // a RADOS object for it and do not flag it in the object map.  This keeps
  // the parent pool sparse, matching the read- and write-triggered fetch
  // paths.  The next read will re-fetch from S3 (small cost relative to the
  // RADOS space we save for VM images that are mostly zero on the tail).
  if (m_data_bl.is_zero()) {
    dout(10) << "object " << m_object_no << " is all-zero, skipping write_full + map update" << dendl;
    release_lock();
    return;
  }

  Context* ctx = librbd::util::create_context_callback<ObjectBackfillRequest, &ObjectBackfillRequest::handle_write_rados>(this);

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
    release_lock();
    return;
  }

  dout(10) << "RADOS write complete, updating object map" << dendl;
  update_object_map();
}

void ObjectBackfillRequest::update_object_map() {
  dout(15) << dendl;

  // Update the parent image's object map via direct RADOS operation.
  // build_update_op exists for callers like us that don't hold the image's
  // ExclusiveLock and therefore can't use the in-memory ObjectMap path.
  std::string object_map_name = librbd::ObjectMap<>::object_map_name(
    m_image_id, CEPH_NOSNAP);

  librados::ObjectWriteOperation map_op;
  librbd::ObjectMap<>::build_update_op(
    &map_op, m_object_no, m_object_no + 1,
    OBJECT_EXISTS, boost::optional<uint8_t>());

  Context* ctx = librbd::util::create_context_callback<ObjectBackfillRequest, &ObjectBackfillRequest::handle_update_object_map>(this);
  librados::AioCompletion* rados_completion =
    librbd::util::create_rados_callback(ctx);

  // aio_operate() returns non-zero only if the request cannot be submitted at
  // all (e.g., connection broken).  In practice this should never happen at
  // this stage; assert for consistency with write_rados() above.
  int r = m_parent_ioctx.aio_operate(object_map_name, rados_completion, &map_op);
  ceph_assert(r == 0);
  rados_completion->release();
}

void ObjectBackfillRequest::handle_update_object_map(int r) {
  dout(15) << "r=" << r << dendl;

  if (r == -ENOENT) {
    // The object map RADOS object does not exist — image was created without
    // the object_map feature, or the map has not been initialised yet.
    // The data write succeeded; skip the map update silently.
    dout(10) << "object map not present for image " << m_image_id
             << ", skipping update" << dendl;
    r = 0;
  }
  if (r < 0) {
    derr << "object map update failed: " << cpp_strerror(r) << dendl;
    m_ret_val = r;
    release_lock();
    return;
  }

  dout(10) << "object map updated, releasing lock" << dendl;
  m_ret_val = 0;
  release_lock();
}

void ObjectBackfillRequest::release_lock() {
  dout(15) << dendl;

  Context* ctx = librbd::util::create_context_callback<ObjectBackfillRequest, &ObjectBackfillRequest::handle_release_lock>(this);

  // Release the lock
  librados::ObjectWriteOperation op;
  rados::cls::lock::unlock(&op, m_lock_name, m_lock_cookie);

  librados::AioCompletion* rados_completion =
    librbd::util::create_rados_callback(ctx);

  int r = m_parent_ioctx.aio_operate(m_lock_oid, rados_completion, &op);
  ceph_assert(r == 0);
  rados_completion->release();
}

void ObjectBackfillRequest::handle_release_lock(int r) {
  dout(15) << "r=" << r << dendl;

  if (r == -ENOENT) {
    // The lock was already gone.  The most common cause: a CopyupRequest
    // (client COW) called rados::cls::lock::break_lock() on the sentinel
    // while this ObjectBackfillRequest was mid-write.  The write still
    // completed successfully (write_full is idempotent — both parties wrote
    // the same S3 data).  This is the normal preemption path; log at debug
    // only.
    dout(10) << "lock already released (client I/O preempted daemon write); "
             << "write completed successfully regardless" << dendl;
  } else if (r < 0) {
    dout(5) << "failed to release lock: " << cpp_strerror(r) << dendl;
    // Continue anyway — lock auto-expires via the configured timeout.
  } else {
    dout(10) << "lock released" << dendl;
  }

  finish(m_ret_val);
}

// Caller invariant: every code path that reaches finish() has either
// (a) never acquired the cls_lock (handle_acquire_lock's EBUSY / error
// branches call finish() before m_lock_acquired is set), or (b) released
// it through release_lock() / handle_release_lock() before getting here.
// The state machine has no other entry, so finish() does not need a
// fallback unlock — m_finished is the sole contract a duplicate caller
// must respect.
void ObjectBackfillRequest::finish(int r) {
  dout(10) << "r=" << r << dendl;

  {
    Mutex::Locker locker(m_lock);
    if (m_finished) {
      dout(5) << "already finished, ignoring duplicate finish call" << dendl;
      return;
    }
    m_finished = true;
    m_ret_val = r;
  }

  m_on_finish->complete(r);
  delete this;
}


} // namespace backfill
} // namespace rbd
