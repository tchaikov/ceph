// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/AsyncWritebackThrottler.h"
#include "librbd/Types.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"
#include "include/stringify.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/ceph_context.h"
#include "cls/lock/cls_lock_client.h"
#include <boost/optional.hpp>
#include <mutex>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
// Prefix all messages from this TU with a stable substring so
// log-grepping (e.g., test-s3-dedup-read.sh) can match on
// "async_writeback:" regardless of thread id or callsite.  Both the
// throttler and its detached state machine live in this file and
// share the same prefix root.
#define dout_prefix *_dout << "librbd::io::async_writeback: "

namespace librbd {
namespace io {

// Forward-declared in the header as a private nested class.  Defined here
// so callers don't need to drag in cls_lock_client / ObjectMap headers.
//
// One instance per accepted try_submit.  Owns:
//   - a copy of the parent IoCtx (so async ops survive the originating
//     ImageCtx going away)
//   - the bufferlist of S3-fetched data
//   - a unique lock cookie ("awb-<addr>-<obj_no>") that distinguishes this
//     writer from BACKFILL_LOCK_COOKIE_PREFIX (backfill daemon),
//     "<image_id>_r_..." (ObjectReadRequest reader), and "<image_id>_..."
//     (CopyupRequest writer) when list_lock_holders enumerates holders
//
// State flow: acquire_lock → write_full → update_object_map → unlock → finish
// On any cls_lock failure (EBUSY = peer already doing the same writeback;
// timeout = lock auto-expired; IO error = transient), we drop silently.
// The throttler's counters decrement in finish() regardless of outcome.
class AsyncWritebackThrottler::WritebackRequest {
public:
  WritebackRequest(CephContext* cct,
                   AsyncWritebackThrottler* throttler,
                   librados::IoCtx& parent_ioctx,
                   std::string parent_oid,
                   uint64_t object_no,
                   ceph::bufferlist&& data,
                   std::string image_id)
    : m_cct(cct),
      m_throttler(throttler),
      m_parent_ioctx(parent_ioctx),  // IoCtx copy (refcounted internally)
      m_parent_oid(std::move(parent_oid)),
      m_object_no(object_no),
      m_image_id(std::move(image_id)),
      m_data(std::move(data)),
      m_bytes_at_submit(m_data.length()) {
    // Sentinel oid: same convention as CopyupRequest, ObjectReadRequest,
    // and ObjectBackfillRequest — all four parties coordinate on the
    // same ".s3lk" sentinel so cls_lock contention works across them.
    m_lock_oid = m_parent_oid + S3_FETCH_LOCK_SENTINEL_SUFFIX;

    // "awb-" prefix lets diagnostic tooling (and the future
    // list_lock_holders classifier in any caller) identify async-writeback
    // holders.  Address + object_no makes it unique per process per object.
    m_lock_cookie = std::string("awb-") +
                    stringify(reinterpret_cast<uintptr_t>(this)) + "-" +
                    stringify(object_no);
  }

  void send() {
    acquire_lock();
  }

private:
  // Short lock duration: the SM completes in ~3 RTTs (~100-300 ms).  If
  // we crash mid-flight, the lock auto-expires within 5 s so peers
  // aren't blocked beyond the natural failure-detection window.
  static constexpr uint32_t LOCK_DURATION_SECONDS = 5;

  void acquire_lock() {
    ldout(m_cct, 15) << "obj=" << m_object_no << " oid=" << m_parent_oid << dendl;

    librados::ObjectWriteOperation op;
    rados::cls::lock::lock(
        &op, S3_FETCH_LOCK_NAME, LOCK_EXCLUSIVE,
        m_lock_cookie, S3_FETCH_LOCK_TAG, "async writeback",
        utime_t(LOCK_DURATION_SECONDS, 0), 0);

    auto on_finish = util::create_context_callback<
        WritebackRequest, &WritebackRequest::handle_acquire_lock>(this);
    auto rados_completion = util::create_rados_callback(on_finish);
    int r = m_parent_ioctx.aio_operate(m_lock_oid, rados_completion, &op);
    ceph_assert(r == 0);
    rados_completion->release();
  }

  void handle_acquire_lock(int r) {
    ldout(m_cct, 15) << "r=" << r << dendl;
    if (r < 0) {
      // EBUSY / EEXIST: another peer is doing this exact write_full
      // right now — that's the cross-process dedup signal.  Anything else
      // is a transient cluster issue.  Either way, drop silently; the
      // backfill daemon rescan covers it.
      ldout(m_cct, 10) << "lock not acquired (" << cpp_strerror(r)
                       << "); dropping writeback for obj=" << m_object_no << dendl;
      finish();
      return;
    }
    m_lock_held = true;
    write_full();
  }

  void write_full() {
    // Mirror ObjectBackfillRequest: skip all-zero objects so the parent
    // pool stays sparse for VM images mostly-zero on the tail.
    if (m_data.is_zero()) {
      ldout(m_cct, 10) << "obj=" << m_object_no
                       << " is all-zero, skipping write_full + obj_map" << dendl;
      release_lock();
      return;
    }

    // Log at 10 so dedup-{read,writeback}.sh's grep at --debug-rbd=10
    // can count successful write_fulls.  This is the key cross-process
    // dedup signal: with N concurrent writebacks contending on cls_lock,
    // exactly 1 should reach write_full and N-1 should EBUSY-drop above.
    ldout(m_cct, 10) << "write_full " << m_data.length()
                     << " bytes to " << m_parent_oid << dendl;

    librados::ObjectWriteOperation op;
    op.write_full(m_data);

    auto on_finish = util::create_context_callback<
        WritebackRequest, &WritebackRequest::handle_write_full>(this);
    auto rados_completion = util::create_rados_callback(on_finish);
    int r = m_parent_ioctx.aio_operate(m_parent_oid, rados_completion, &op);
    ceph_assert(r == 0);
    rados_completion->release();
  }

  void handle_write_full(int r) {
    ldout(m_cct, 15) << "r=" << r << dendl;
    if (r < 0) {
      lderr(m_cct) << "write_full failed: " << cpp_strerror(r)
                   << " (obj=" << m_object_no << "); releasing lock" << dendl;
      release_lock();
      return;
    }
    update_object_map();
  }

  void update_object_map() {
    ldout(m_cct, 15) << "obj=" << m_object_no << dendl;

    // build_update_op is the off-image-ExclusiveLock path: takes a
    // RADOS op and appends a cls call that sets the bit for our
    // object_no.  Same convention as ObjectBackfillRequest.
    std::string object_map_oid =
        ObjectMap<>::object_map_name(m_image_id, CEPH_NOSNAP);

    librados::ObjectWriteOperation op;
    ObjectMap<>::build_update_op(&op, m_object_no, m_object_no + 1,
                                 OBJECT_EXISTS, boost::optional<uint8_t>());

    auto on_finish = util::create_context_callback<
        WritebackRequest, &WritebackRequest::handle_update_object_map>(this);
    auto rados_completion = util::create_rados_callback(on_finish);
    int r = m_parent_ioctx.aio_operate(object_map_oid, rados_completion, &op);
    ceph_assert(r == 0);
    rados_completion->release();
  }

  void handle_update_object_map(int r) {
    ldout(m_cct, 15) << "r=" << r << dendl;
    if (r == -ENOENT) {
      // Object_map RADOS object doesn't exist — image lacks the
      // object_map feature, or it's not initialised yet.  The data
      // write_full already succeeded; skip the map update silently.
      ldout(m_cct, 10) << "object_map absent for image " << m_image_id
                       << "; skipping bit update" << dendl;
    } else if (r < 0) {
      lderr(m_cct) << "object_map update failed: " << cpp_strerror(r)
                   << " (obj=" << m_object_no << "); proceeding to unlock"
                   << dendl;
    }
    release_lock();
  }

  void release_lock() {
    if (!m_lock_held) {
      finish();
      return;
    }
    ldout(m_cct, 15) << "unlocking " << m_lock_oid << dendl;

    librados::ObjectWriteOperation op;
    rados::cls::lock::unlock(&op, S3_FETCH_LOCK_NAME, m_lock_cookie);

    auto on_finish = util::create_context_callback<
        WritebackRequest, &WritebackRequest::handle_release_lock>(this);
    auto rados_completion = util::create_rados_callback(on_finish);
    int r = m_parent_ioctx.aio_operate(m_lock_oid, rados_completion, &op);
    ceph_assert(r == 0);
    rados_completion->release();
  }

  void handle_release_lock(int r) {
    if (r == -ENOENT) {
      // Lock already gone — typically a CopyupRequest or backfill peer
      // called break_lock during our write_full.  The write_full itself
      // succeeded (write_full is idempotent on the same data).  Match
      // ObjectBackfillRequest's debug-level logging here.
      ldout(m_cct, 10) << "lock already released (peer preempted); "
                       << "write_full completed regardless" << dendl;
    } else if (r < 0) {
      ldout(m_cct, 5) << "unlock failed: " << cpp_strerror(r)
                      << " (lock will auto-expire)" << dendl;
    }
    finish();
  }

  void finish() {
    ldout(m_cct, 15) << "obj=" << m_object_no << dendl;
    m_throttler->on_writeback_complete(m_bytes_at_submit);
    delete this;
  }

  CephContext* m_cct;
  AsyncWritebackThrottler* m_throttler;
  librados::IoCtx m_parent_ioctx;
  std::string m_parent_oid;
  std::string m_lock_oid;
  std::string m_lock_cookie;
  uint64_t m_object_no;
  std::string m_image_id;
  ceph::bufferlist m_data;
  const uint64_t m_bytes_at_submit;
  bool m_lock_held = false;
};

namespace {

// Singleton mutex + storage.  Function-local static would also work
// (C++11 mandates thread-safe init), but the explicit pair makes it
// trivial to support per-cct instances later if that becomes useful.
std::mutex g_instance_lock;
std::unique_ptr<AsyncWritebackThrottler> g_instance;

constexpr uint32_t DEFAULT_MAX_CONCURRENT = 8;
constexpr uint64_t DEFAULT_MAX_BYTES_IN_FLIGHT = 32ull * 1024 * 1024;  // 32 MB

} // anonymous namespace

AsyncWritebackThrottler::AsyncWritebackThrottler(CephContext* cct,
                                                 uint32_t max_concurrent,
                                                 uint64_t max_bytes_in_flight)
  : m_cct(cct),
    m_max_concurrent(max_concurrent),
    m_max_bytes_in_flight(max_bytes_in_flight),
    m_lock(util::unique_lock_name(
        "librbd::io::AsyncWritebackThrottler::m_lock", this)) {
  ldout(m_cct, 10) << "max_concurrent=" << m_max_concurrent
                   << " max_bytes_in_flight=" << m_max_bytes_in_flight << dendl;
}

AsyncWritebackThrottler& AsyncWritebackThrottler::instance(CephContext* cct) {
  std::lock_guard<std::mutex> guard(g_instance_lock);
  if (!g_instance) {
    // Config snapshot at first-use.  Subsequent cct passed in is ignored;
    // the process-wide throttle is committed to whatever the first cct's
    // config said.  Acceptable trade-off — the option is admin-tuned at
    // startup, not per-image.
    auto max_concurrent = cct->_conf.get_val<uint64_t>(
        "rbd_s3_async_writeback_max_concurrent");
    auto max_bytes = cct->_conf.get_val<Option::size_t>(
        "rbd_s3_async_writeback_max_bytes_in_flight").value;
    if (max_concurrent == 0) max_concurrent = DEFAULT_MAX_CONCURRENT;
    if (max_bytes == 0) max_bytes = DEFAULT_MAX_BYTES_IN_FLIGHT;
    g_instance.reset(new AsyncWritebackThrottler(
        cct, static_cast<uint32_t>(max_concurrent), max_bytes));
  }
  return *g_instance;
}

bool AsyncWritebackThrottler::try_submit(librados::IoCtx& ioctx,
                                          const std::string& parent_oid,
                                          uint64_t object_no,
                                          ceph::bufferlist&& data,
                                          const std::string& image_id) {
  const uint64_t data_bytes = data.length();

  {
    Mutex::Locker locker(m_lock);
    if (m_in_flight >= m_max_concurrent) {
      ldout(m_cct, 10) << "drop: in_flight " << m_in_flight
                       << " >= max " << m_max_concurrent
                       << " (obj=" << object_no << ")" << dendl;
      return false;
    }
    if (m_bytes_in_flight + data_bytes > m_max_bytes_in_flight) {
      ldout(m_cct, 10) << "drop: bytes_in_flight " << m_bytes_in_flight
                       << " + " << data_bytes
                       << " > max " << m_max_bytes_in_flight
                       << " (obj=" << object_no << ")" << dendl;
      return false;
    }
    ++m_in_flight;
    m_bytes_in_flight += data_bytes;
    // Log at 10 so the accept event is visible at the typical
    // debug-rbd=10 level callers raise during testing; matches the
    // level used by the drop branches above.
    ldout(m_cct, 10) << "accept obj=" << object_no
                     << " (in_flight=" << m_in_flight
                     << "/" << m_max_concurrent
                     << ", bytes=" << m_bytes_in_flight
                     << "/" << m_max_bytes_in_flight << ")" << dendl;
  }

  // Construct outside m_lock to keep critical section tight.  The SM
  // owns its own lifetime (delete this on finish); throttler counters
  // decrement via on_writeback_complete() called from finish().
  auto req = new WritebackRequest(m_cct, this, ioctx, parent_oid,
                                  object_no, std::move(data), image_id);
  req->send();
  return true;
}

void AsyncWritebackThrottler::on_writeback_complete(uint64_t bytes_freed) {
  Mutex::Locker locker(m_lock);
  ceph_assert(m_in_flight > 0);
  ceph_assert(m_bytes_in_flight >= bytes_freed);
  --m_in_flight;
  m_bytes_in_flight -= bytes_freed;
  ldout(m_cct, 10) << "complete (in_flight=" << m_in_flight
                   << ", bytes=" << m_bytes_in_flight << ")" << dendl;
  if (m_in_flight == 0) {
    m_idle_cond.Signal();
  }
}

void AsyncWritebackThrottler::wait_for_idle() {
  Mutex::Locker locker(m_lock);
  while (m_in_flight > 0) {
    ldout(m_cct, 15) << "wait_for_idle: " << m_in_flight
                     << " in-flight" << dendl;
    m_idle_cond.Wait(m_lock);
  }
  ldout(m_cct, 15) << "wait_for_idle: all complete" << dendl;
}

uint32_t AsyncWritebackThrottler::in_flight() const {
  Mutex::Locker locker(m_lock);
  return m_in_flight;
}

uint64_t AsyncWritebackThrottler::bytes_in_flight() const {
  Mutex::Locker locker(m_lock);
  return m_bytes_in_flight;
}

} // namespace io
} // namespace librbd
