// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_BACKFILL_DAEMON_H
#define CEPH_RBD_BACKFILL_DAEMON_H

#include "Types.h"
#include "include/rados/librados.hpp"
#include "common/Mutex.h"
#include "common/Cond.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>

class CephContext;
class ContextWQ;
class SafeTimer;
class ThreadPool;

namespace rbd {
namespace backfill {

class BackfillThrottler;
class ImageBackfiller;

struct Threads {
  std::unique_ptr<ThreadPool> thread_pool;
  std::unique_ptr<ContextWQ> work_queue;
  std::unique_ptr<SafeTimer> timer;
  Mutex timer_lock;

  explicit Threads(CephContext *cct);
  ~Threads();

  Threads(const Threads&) = delete;
  Threads& operator=(const Threads&) = delete;
};

class BackfillDaemon {
public:
  BackfillDaemon(CephContext *cct);
  ~BackfillDaemon();

  BackfillDaemon(const BackfillDaemon&) = delete;
  BackfillDaemon& operator=(const BackfillDaemon&) = delete;

  int init();
  void run();
  void shutdown();

private:
  int connect_to_cluster();

  // Scan the cluster for `rbd backfill schedule`d images and claim any that
  // aren't already in m_image_backfillers.  Newly-claimed specs are appended
  // to <new_specs>.  Idempotent: scheduled images already being processed
  // are skipped (they carry the BACKFILL_SCHED_IN_PROGRESS marker).
  int discover_scheduled_images(std::vector<ImageSpec>* new_specs);

  // Spawn an ImageBackfiller for each spec in <specs>.  Caller is responsible
  // for ensuring no spec is already in m_image_backfillers.
  int start_image_backfillers(const std::vector<ImageSpec>& specs);

  // Periodic rescan: discover newly scheduled images and start backfillers.
  // schedule_rescan arms a one-shot timer event and MUST be called with
  // m_threads->timer_lock held — Ceph's SafeTimer::add_event_after asserts
  // the lock and timer callbacks themselves run with the lock held
  // (safe_callbacks=true).
  void schedule_rescan();
  // Public entry; acquires no lock.  Used by tests / external callers that
  // want to force an immediate rescan independent of the timer.
  void rescan_tick();
  // Internal: do one pass of the rescan work without re-arming the timer.
  // Caller must NOT hold m_threads->timer_lock — RADOS scan can take seconds.
  void do_rescan_tick();

  void handle_image_complete(const ImageSpec& spec, int r);

  CephContext *m_cct;
  std::unique_ptr<Threads> m_threads;
  std::unique_ptr<BackfillThrottler> m_throttler;

  librados::Rados m_rados;

  std::map<ImageSpec, std::unique_ptr<ImageBackfiller>> m_image_backfillers;

  // Periodic rescan state.  When m_rescan_interval == 0, rescan is disabled
  // and the daemon reverts to legacy one-shot semantics (exits when all
  // initially-scheduled images finish).  When >0, the daemon stays alive
  // until shutdown() is called explicitly (signal/manual stop).
  uint32_t m_rescan_interval = 0;
  Context* m_rescan_event = nullptr;  // pending timer event under timer_lock

  Mutex m_lock;
  Cond m_cond;
  bool m_stopping = false;
  std::atomic<bool> m_shutdown{false};
  // shutdown() runs once on whichever thread arrives first (signal handler
  // or main after run() returns).  The other caller observes m_shutdown=true
  // and must WAIT for the in-progress shutdown to finish — otherwise main's
  // ~BackfillDaemon can race the async thread that's still inside shutdown,
  // destructing m_threads/m_throttler underneath it (and tripping the
  // ceph_assert(!m_threads) in the destructor).
  bool m_shutdown_done = false;
  Cond m_shutdown_cond;
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_RBD_BACKFILL_DAEMON_H
