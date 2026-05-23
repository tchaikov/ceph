// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_ASYNC_WRITEBACK_THROTTLER_H
#define CEPH_LIBRBD_IO_ASYNC_WRITEBACK_THROTTLER_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "include/rados/librados.hpp"
#include "common/Mutex.h"
#include "common/Cond.h"
#include <string>

class CephContext;

namespace librbd {
namespace io {

// AsyncWritebackThrottler — per-process gate for detached parent-cache
// writebacks fired by ObjectReadRequest and CopyupRequest after they
// return data to the client.
//
// Design intent: the client's read/write critical path returns AS SOON AS
// the S3 GET completes; the work of populating RADOS (cls_lock + write_full
// + obj_map update + cls_unlock) runs detached so the client doesn't wait
// on RADOS coordination overhead.
//
// Without a throttle, a burst of concurrent reads could fire N parallel
// 4 MB write_fulls and starve foreground client I/O on the OSDs.  This
// class bounds:
//   - the number of in-flight detached writeback state machines, and
//   - the aggregate bytes those state machines hold (covers the
//     per-machine 4 MB bufferlist staying alive until write_full lands)
//
// Drop semantics (NOT queue): when either limit is hit, try_submit
// returns false and the caller silently discards the data.  Dropping is
// correct because:
//   - the parent cache is opportunistic, never authoritative
//   - the backfill daemon's rescan eventually populates dropped objects
//   - queueing would let memory grow unbounded under sustained read load,
//     and stale queued writebacks are less useful than fresh ones anyway
//
// Per-process scope: a single instance shared across every ImageCtx in
// the process.  Matches the deployment model (1 VM = 1 librbd client =
// 1 process) and keeps the throttle budget interpretable.
//
// Lifetime: the first instance() call creates the singleton; later calls
// return the same instance regardless of the cct they pass.  The
// singleton is never destroyed (lives for the process lifetime).
class AsyncWritebackThrottler {
public:
  AsyncWritebackThrottler(const AsyncWritebackThrottler&) = delete;
  AsyncWritebackThrottler& operator=(const AsyncWritebackThrottler&) = delete;

  // Per-process singleton accessor.  Thread-safe.
  static AsyncWritebackThrottler& instance(CephContext* cct);

  // Returns true if accepted (caller hands off data ownership via move).
  // Returns false if throttle is full (caller must drop the data; the
  // cache will be populated by backfill rescan or a later reader).
  //
  // On accept, this method spawns a detached state machine that:
  //   1. acquires cls_lock on (parent_oid + S3_FETCH_LOCK_SENTINEL_SUFFIX)
  //   2. on success → write_full(parent_oid, data) + obj_map update
  //   3. cls_unlock (fire-and-forget)
  //   4. decrements throttle counters
  //
  // On any error from the cls_lock acquire (EBUSY, timeout, IO error),
  // the state machine drops silently and decrements counters.  The
  // EBUSY case is the expected "another peer is doing this exact
  // write_full" path — the lock is the cross-process dedup signal.
  //
  // Wired up in commit 2 (ObjectReadRequest) and commit 3 (CopyupRequest).
  // In commit 1 (this class in isolation), no caller exists yet; the
  // unit test exercises the counting / drop logic via a stubbed SM.
  bool try_submit(librados::IoCtx& ioctx,
                  const std::string& parent_oid,
                  uint64_t object_no,
                  ceph::bufferlist&& data,
                  const std::string& image_id);

  // Blocks until every in-flight detached writeback has terminated
  // (success, EBUSY, timeout, error).  Used by:
  //   - ImageCtx::close paths that want to ensure no detached SM holds
  //     an IoCtx alias past the close
  //   - unit tests that need to observe the terminal state
  void wait_for_idle();

  // Snapshots for diagnostics (visible via the perf-counters / admin
  // socket once wired up by callers).  Cheap (mutex acquire only).
  uint32_t in_flight() const;
  uint64_t bytes_in_flight() const;

private:
  AsyncWritebackThrottler(CephContext* cct,
                          uint32_t max_concurrent,
                          uint64_t max_bytes_in_flight);

  // Called by the detached state machine when it terminates (any cause).
  // Decrements counters and signals m_idle_cond if everything is done.
  void on_writeback_complete(uint64_t bytes_freed);

  // Owns and runs the detached cls_lock + write_full + obj_map + unlock
  // state machine.  Defined in the .cc; one instance per accepted submit.
  class WritebackRequest;
  friend class WritebackRequest;

  CephContext* m_cct;
  const uint32_t m_max_concurrent;
  const uint64_t m_max_bytes_in_flight;

  // Single mutex for counters + idle cond.  Contention is low: each
  // try_submit takes the lock for a few microseconds to check + bump
  // counters before handing off to the async cls_lock acquire.
  mutable Mutex m_lock;
  Cond m_idle_cond;
  uint32_t m_in_flight = 0;
  uint64_t m_bytes_in_flight = 0;
};

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_ASYNC_WRITEBACK_THROTTLER_H
