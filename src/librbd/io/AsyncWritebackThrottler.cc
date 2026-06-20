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
    // Hold a CephContext refcount: this SM can outlive the throttler
    // when AsyncWritebackThrottler::wait_for_idle hits its timeout
    // and returns leaving in-flight SMs running.  If ~ImageCtx /
    // ~Rados then drop the last cct ref, every ldout(m_cct, ...) in
    // this SM would UAF.  Holding our own ref keeps cct alive until
    // ~WritebackRequest.
    m_cct->get();
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

  ~WritebackRequest() {
    m_cct->put();
  }

  void send() {
    acquire_lock();
  }

private:
  // Lock duration must cover the worst-case write_full + object_map
  // cls + unlock latency.  ~3 RTTs is typical (100-300 ms) but a 4 MB
  // write to a journal-pressured OSD under 8-way throttler contention
  // can take many seconds.  If the lock auto-expires mid write_full,
  // a peer can acquire and issue ITS OWN write_full to the same OID --
  // two writers racing on the parent object risk mixed bytes if the
  // OSD interleaves them.  30 s matches the conventional Ceph cls_lock
  // duration for multi-MB ops; if we genuinely crash, peers still
  // recover within one failure-detection window (default mon timeout).
  static constexpr uint32_t LOCK_DURATION_SECONDS = 30;

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

    ldout(m_cct, 15) << "obj=" << m_object_no << " populating "
                     << m_data.length() << " bytes to " << m_parent_oid
                     << " (exclusive create)" << dendl;

    // Idempotent dedup: create(exclusive) makes the whole op fail -EEXIST if
    // the object is already populated (by a peer writeback, CopyupRequest, or
    // the backfill daemon).  The cls_lock only dedups writebacks that contend
    // SIMULTANEOUSLY; a writeback that acquires the lock AFTER a peer already
    // wrote and released would otherwise re-write the same immutable bytes.
    // The exclusive create covers that SEQUENTIAL (staggered-submission) case,
    // so exactly one write_full lands per parent object regardless of timing.
    // The immutable S3 source guarantees an existing object holds identical
    // bytes, so skipping the redundant write is safe.
    librados::ObjectWriteOperation op;
    op.create(true);
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
    if (r == -EEXIST) {
      // Dedup outcome (see write_full's exclusive-create comment): a peer
      // already populated the object, so no write landed here.  Still set the
      // object_map bit idempotently, and do NOT emit the counted write_full
      // log line -- no write happened.
      ldout(m_cct, 10) << "obj=" << m_object_no << " already populated in "
                       << m_parent_oid
                       << "; skipped redundant write_full (cross-writer dedup)"
                       << dendl;
      update_object_map();
      return;
    }
    if (r < 0) {
      lderr(m_cct) << "write_full failed: " << cpp_strerror(r)
                   << " (obj=" << m_object_no << "); releasing lock" << dendl;
      release_lock();
      return;
    }
    // Counted dedup signal -- emitted ONLY when a real write landed (grep'd by
    // dedup-{read,writeback}.sh at --debug-rbd=10).  With N contending
    // writebacks, exactly one reaches here per parent object.
    ldout(m_cct, 10) << "write_full " << m_data.length()
                     << " bytes to " << m_parent_oid << dendl;
    m_write_full_succeeded = true;
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
      // Lock already gone -- typically a peer (CopyupRequest, backfill)
      // called break_lock during our write_full, OR our cls_lock TTL
      // auto-expired before we got around to the unlock op.  Whether
      // the parent oid was actually written depends on whether
      // write_full itself succeeded: m_write_full_succeeded captures
      // that.  Logging the truth here avoids misleading operators who
      // grep for "completed regardless" after data-loss incidents.
      if (m_write_full_succeeded) {
        ldout(m_cct, 10) << "lock already released (peer preempted); "
                         << "write_full had completed successfully" << dendl;
      } else {
        ldout(m_cct, 10) << "lock already released; write_full did NOT "
                         << "complete (lderr above for details)" << dendl;
      }
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
  bool m_write_full_succeeded = false;
};

namespace {

// Per-cct instance map.  Each CephContext (one per librados::Rados in
// the typical librbd consumer) gets its own throttler with independent
// in_flight counters and config snapshot.  Replaces the c1 singleton
// design (process-wide; addressed review findings C2, New6, and the
// C1 UAF — see header design comment).
std::mutex g_instances_lock;
std::unordered_map<CephContext*,
                   std::unique_ptr<AsyncWritebackThrottler>> g_instances;

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
  // Pin the CephContext so the throttler (and its long-lived
  // ldout(m_cct, ...) calls) stay valid even if all ImageCtxs sharing
  // this cct are destroyed.  Without this, g_instances entries keep
  // the throttler alive but its m_cct can dangle once the last Rados
  // handle drops its ref; instance() for a future cct that happens to
  // get the same address would also return this stale entry with a
  // freed m_cct.  ~AsyncWritebackThrottler is never called (entries
  // live until cleanup_for_test or process exit), so this ref is a
  // one-per-cct ground-state, not a leak that grows.
  m_cct->get();
  ldout(m_cct, 10) << "max_concurrent=" << m_max_concurrent
                   << " max_bytes_in_flight=" << m_max_bytes_in_flight << dendl;
}

AsyncWritebackThrottler::~AsyncWritebackThrottler() {
  // Only reached via cleanup_for_test or process-exit static teardown.
  m_cct->put();
}

AsyncWritebackThrottler& AsyncWritebackThrottler::instance(CephContext* cct) {
  std::lock_guard<std::mutex> guard(g_instances_lock);
  auto it = g_instances.find(cct);
  if (it == g_instances.end()) {
    // Config snapshot at first-use for THIS cct.  Different ccts can
    // therefore have different snapshots — what cct A saw when its
    // first ImageCtx opened, and what cct B saw when its first
    // ImageCtx opened.  ceph_config set after either cct's first use
    // is silently ignored for that cct until process restart; this is
    // a known limitation documented on rbd_s3_async_writeback_max_*.
    auto max_concurrent = cct->_conf.get_val<uint64_t>(
        "rbd_s3_async_writeback_max_concurrent");
    auto max_bytes = cct->_conf.get_val<Option::size_t>(
        "rbd_s3_async_writeback_max_bytes_in_flight").value;
    // options.cc declares min=1 for these so 0 is unreachable in
    // production -- but keep a defensive default for unit tests that
    // poke the throttler directly without going through Ceph config
    // validation, and for any future flag rename that briefly leaves
    // the new key absent.
    if (max_concurrent == 0) max_concurrent = DEFAULT_MAX_CONCURRENT;
    if (max_bytes == 0) max_bytes = DEFAULT_MAX_BYTES_IN_FLIGHT;
    auto inserted = g_instances.emplace(
        cct,
        std::unique_ptr<AsyncWritebackThrottler>(new AsyncWritebackThrottler(
            cct, static_cast<uint32_t>(max_concurrent), max_bytes)));
    it = inserted.first;
  }
  return *it->second;
}

void AsyncWritebackThrottler::cleanup_for_test() {
  std::lock_guard<std::mutex> guard(g_instances_lock);
  g_instances.clear();
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
  //
  // Roll back the counter bumps if construction throws (bad_alloc, or
  // an internal allocation in the stringify/concat in the ctor).
  // Without rollback, m_in_flight monotonically drifts up; eventually
  // every try_submit hits the cap, and wait_for_idle blocks forever
  // because no SM exists to call on_writeback_complete.
  WritebackRequest *req = nullptr;
  try {
    req = new WritebackRequest(m_cct, this, ioctx, parent_oid,
                               object_no, std::move(data), image_id);
  } catch (...) {
    Mutex::Locker locker(m_lock);
    ceph_assert(m_in_flight > 0);
    --m_in_flight;
    m_bytes_in_flight -= data_bytes;
    if (m_in_flight == 0) {
      m_idle_cond.Signal();
    }
    throw;
  }
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

void AsyncWritebackThrottler::wait_for_idle(uint64_t deadline_ms) {
  // Resolve deadline_ms == 0 to the configured default so callers
  // (typically ~ImageCtx) get the bounded wait without having to pass
  // the option name through.  Pre-fix behaviour (unbounded) is
  // recovered only when the operator explicitly sets the option to 0.
  if (deadline_ms == 0) {
    deadline_ms = m_cct->_conf.get_val<uint64_t>(
        "rbd_s3_async_writeback_wait_for_idle_timeout_ms");
  }

  Mutex::Locker locker(m_lock);
  if (deadline_ms == 0) {
    // Operator explicitly opted into the unbounded wait.
    while (m_in_flight > 0) {
      ldout(m_cct, 15) << "wait_for_idle: " << m_in_flight
                       << " in-flight (unbounded)" << dendl;
      m_idle_cond.Wait(m_lock);
    }
  } else {
    // Use a monotonic clock for the deadline so an NTP backward step
    // can't drive the cutoff into the past and turn the first
    // WaitInterval into an immediate ETIMEDOUT (which would skip the
    // drain entirely while in-flight SMs are still running -- and the
    // destructor would tear down the IoCtx under them).  Integer-only
    // arithmetic also avoids the double-precision rounding that
    // utime_t::set_from_double does for nanos/1e9.
    auto start = ceph::coarse_mono_clock::now();
    auto deadline = start + std::chrono::milliseconds(deadline_ms);
    while (m_in_flight > 0) {
      auto now = ceph::coarse_mono_clock::now();
      if (now >= deadline) {
        // Stuck OSD / partitioned cluster / cls_lock holder unreachable.
        // The still-in-flight SMs continue running; they cleanup via
        // librados's own op_timeout (if configured) or via process
        // exit.  Each SM holds its own CephContext ref + IoCtx copy,
        // so the SM can safely outlive ~ImageCtx and ~Rados.  Log
        // loudly so the operator can correlate with cluster issues.
        lderr(m_cct) << "wait_for_idle deadline " << deadline_ms
                     << " ms exceeded with " << m_in_flight
                     << " writeback(s) still in flight; returning anyway"
                     << dendl;
        break;
      }
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - now);
      ldout(m_cct, 15) << "wait_for_idle: " << m_in_flight
                       << " in-flight (" << remaining.count()
                       << " ms remaining of " << deadline_ms << ")" << dendl;
      // WaitInterval is internally wall-clock-based (Cond.h:79), but the
      // relative-duration form recomputes the wall-time cutoff on each
      // call from the current wall time, so a single NTP step costs at
      // most one iteration's worth of extra wait, not the full drain.
      m_idle_cond.WaitInterval(m_lock, remaining);
    }
  }
  ldout(m_cct, 15) << "wait_for_idle: in_flight=" << m_in_flight << dendl;
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
