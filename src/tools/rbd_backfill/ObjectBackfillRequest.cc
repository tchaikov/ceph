// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ObjectBackfillRequest.h"
#include "BackfillDaemon.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "cls/lock/cls_lock_client.h"
#include "cls/rbd/cls_rbd_client.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"
#include <boost/optional.hpp>
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
  const std::string& image_id,
  CephContext* cct,
  Threads* threads,
  Context* on_finish)
  : m_parent_ioctx(parent_ioctx),  // Copy IoCtx - safe for async use
    m_parent_oid(parent_oid),
    m_object_no(object_no),
    m_image_id(image_id),
    m_threads(threads),
    m_cct(cct),
    m_on_finish(on_finish),
    m_lock("ObjectBackfillRequest::m_lock"),
    m_state(STATE_INIT),
    m_ret_val(0),
    m_cancel_flag(false),
    m_lock_acquired(false),
    m_finished(false),
    m_data_bl(data) {  // Store pre-fetched data

  // Generate unique lock cookie using thread ID and timestamp
  std::stringstream ss;
  ss << "backfill-" << pthread_self() << "-" << ceph_clock_now();
  m_lock_cookie = ss.str();

  // Lock name is the object name
  m_lock_name = "s3_fetch_lock";
  m_lock_tag = "";  // No tag for exclusive locks

  // Sentinel lock object: must match CopyupRequest::m_parent_lock_oid
  // (m_parent_oid + ".s3lk").  Using a separate sentinel prevents the
  // cls_lock side-effect of creating the data object as an empty RADOS
  // object, and ensures mutual exclusion with COW operations from child images.
  m_lock_oid = m_parent_oid + ".s3lk";
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

  // Update the parent image's object map via direct RADOS operation.
  // We use cls_rbd directly rather than the in-memory ObjectMap because
  // the backfill daemon does not hold the image's exclusive lock.
  std::string object_map_name = librbd::ObjectMap<>::object_map_name(
    m_image_id, CEPH_NOSNAP);

  librados::ObjectWriteOperation map_op;
  librbd::cls_client::object_map_update(
    &map_op, m_object_no, m_object_no + 1,
    OBJECT_EXISTS, boost::optional<uint8_t>());

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_update_object_map);
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

  Context* ctx = new C_Request(
    this, &ObjectBackfillRequest::handle_release_lock);

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

  // If we still hold the lock, release it before completing
  if (need_release_lock) {
    dout(10) << "releasing lock from finish()" << dendl;

    // Release lock synchronously to ensure it's released before we complete
    librados::ObjectWriteOperation op;
    rados::cls::lock::unlock(&op, m_lock_name, m_lock_cookie);

    int unlock_r = m_parent_ioctx.operate(m_lock_oid, &op);
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


} // namespace backfill
} // namespace rbd
