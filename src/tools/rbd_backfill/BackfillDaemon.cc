// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "BackfillDaemon.h"
#include "Types.h"
#include "BackfillThrottler.h"
#include "ImageBackfiller.h"
#include "include/rados/librados.hpp"
#include "include/rbd/librbd.hpp"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "common/Timer.h"
#include "librbd/Utils.h"
#include "cls/rbd/cls_rbd_client.h"

#define dout_context m_cct
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd::backfill::BackfillDaemon: " << this << " " << __func__ << ": "

namespace rbd {
namespace backfill {

Threads::Threads(CephContext *cct)
  : timer_lock("rbd::backfill::Threads::timer_lock") {
  thread_pool.reset(new ThreadPool(cct, "rbd_backfill", "tp_rbd_backfill",
                                    cct->_conf.get_val<uint64_t>("rbd_op_threads")));
  thread_pool->start();

  work_queue.reset(new ContextWQ("rbd_backfill_wq",
                                 cct->_conf.get_val<uint64_t>("rbd_op_thread_timeout"),
                                 thread_pool.get()));

  Mutex::Locker timer_locker(timer_lock);
  timer.reset(new SafeTimer(cct, timer_lock, true));
  timer->init();
}

Threads::~Threads() {
  {
    Mutex::Locker timer_locker(timer_lock);
    timer->shutdown();
  }
  timer.reset();

  work_queue->drain();
  work_queue.reset();

  thread_pool->stop();
}

BackfillDaemon::BackfillDaemon(CephContext *cct)
  : m_cct(cct),
    m_lock(librbd::util::unique_lock_name("rbd::backfill::BackfillDaemon::m_lock", this)) {
  dout(10) << dendl;
}

BackfillDaemon::~BackfillDaemon() {
  dout(10) << dendl;

  // Cleanup should happen in shutdown()
  ceph_assert(!m_threads);
  ceph_assert(!m_throttler);
  ceph_assert(m_image_backfillers.empty());
}

int BackfillDaemon::init() {
  dout(10) << dendl;

  int r = connect_to_cluster();
  if (r < 0) {
    derr << "failed to connect to cluster: " << cpp_strerror(r) << dendl;
    return r;
  }

  m_threads = std::make_unique<Threads>(m_cct);
  m_throttler = std::make_unique<BackfillThrottler>(m_cct, m_threads->work_queue.get());

  m_rescan_interval = m_cct->_conf.get_val<uint64_t>("rbd_backfill_rescan_interval");

  dout(5) << "daemon initialized successfully (rescan_interval="
          << m_rescan_interval << "s, "
          << (m_rescan_interval == 0 ? "one-shot mode" : "continuous mode")
          << ")" << dendl;
  return 0;
}

void BackfillDaemon::run() {
  dout(10) << dendl;

  // Initial discovery — pick up images already scheduled at daemon startup.
  std::vector<ImageSpec> initial_specs;
  int r = discover_scheduled_images(&initial_specs);
  if (r < 0) {
    derr << "failed to discover scheduled images: " << cpp_strerror(r) << dendl;
    return;
  }

  r = start_image_backfillers(initial_specs);
  if (r < 0) {
    derr << "failed to start image backfillers: " << cpp_strerror(r) << dendl;
    return;
  }

  {
    Mutex::Locker locker(m_lock);
    dout(5) << "started " << m_image_backfillers.size() << " image backfiller(s)" << dendl;
  }

  // If rescan is enabled, schedule the first periodic tick.  This keeps the
  // daemon alive past the initial set: subsequent `rbd backfill schedule`
  // invocations are picked up within rescan_interval seconds.
  if (m_rescan_interval > 0) {
    Mutex::Locker timer_locker(m_threads->timer_lock);
    schedule_rescan();
  }

  // Main loop — wait for either:
  //   - explicit shutdown via signal handler / shutdown(), OR
  //   - one-shot completion (m_stopping set in handle_image_complete when
  //     rescan is disabled and all backfillers finish).
  {
    Mutex::Locker locker(m_lock);
    while (!m_stopping) {
      dout(20) << "waiting for shutdown signal" << dendl;
      m_cond.Wait(m_lock);
    }
  }

  dout(5) << "shutdown signal received" << dendl;
}

void BackfillDaemon::shutdown() {
  dout(10) << dendl;

  // Guard against double-shutdown (signal handler + main() both call shutdown).
  // The second caller MUST wait for the first to finish, not return immediately,
  // because ~BackfillDaemon asserts m_threads/m_throttler are reset and the
  // first caller may still be reset()ing them.
  if (m_shutdown.exchange(true)) {
    Mutex::Locker locker(m_lock);
    while (!m_shutdown_done) {
      dout(10) << "another thread is shutting down; waiting for completion" << dendl;
      m_shutdown_cond.Wait(m_lock);
    }
    dout(10) << "shutdown already completed by another thread" << dendl;
    return;
  }

  {
    Mutex::Locker locker(m_lock);
    m_stopping = true;
    m_cond.Signal();
  }

  // Cancel any pending rescan timer event so it does not run during teardown
  // (the timer thread holds timer_lock; do not also hold m_lock here to avoid
  // a lock-order inversion with rescan_tick).
  if (m_threads) {
    Mutex::Locker timer_locker(m_threads->timer_lock);
    if (m_rescan_event) {
      m_threads->timer->cancel_event(m_rescan_event);
      m_rescan_event = nullptr;
    }
  }

  // Move all backfillers out before stopping.  Two requirements pull in
  // opposite directions:
  //
  // 1. m_image_backfillers must be protected by m_lock: a backfiller thread
  //    that finishes naturally (all objects done) calls handle_image_complete()
  //    which acquires m_lock and erases from the map.  Without the lock here
  //    that would be a data race.
  //
  // 2. stop() must be called WITHOUT m_lock: stop() joins the thread, and the
  //    thread calls handle_image_complete() which tries to acquire m_lock.
  //    Holding m_lock across stop() → join() would deadlock.
  //
  // Solution: hold m_lock only for the move+clear, then drop it before stop().
  // Once the map is cleared, handle_image_complete() finds nothing and returns
  // early, so the join() in stop() cannot deadlock.
  std::vector<std::unique_ptr<ImageBackfiller>> to_stop;
  {
    Mutex::Locker locker(m_lock);
    to_stop.reserve(m_image_backfillers.size());
    for (auto& pair : m_image_backfillers) {
      dout(10) << "stopping backfiller for " << pair.first.pool_name
               << "/" << pair.first.image_name << dendl;
      to_stop.push_back(std::move(pair.second));
    }
    m_image_backfillers.clear();
  }
  for (auto& backfiller : to_stop) {
    backfiller->stop();
  }

  // Destroy the ImageBackfillers BEFORE m_threads.reset() / m_rados.shutdown().
  // ~ImageBackfiller calls m_image_ctx->state->close(), which in turn fires
  // ManagedLock::complete_shutdown asynchronously onto librbd's ThreadPool
  // (the tp_librbd worker).  That callback ends up in Watcher::unregister_watch
  // → Mutex::lock on a Watcher mutex that lives inside the ImageCtx.  If we
  // let `to_stop` go out of scope at function-end (i.e. AFTER m_rados.shutdown
  // disconnects the IoCtx the image_ctx was opened against), the in-flight
  // callback runs against a half-destroyed image and Mutex::lock fails its
  // ceph_assert(r == 0) — the daemon then hangs forever in abort()'s event
  // dump because m_rados is already torn down.  Closing here (synchronously,
  // since ImageState::close blocks until its SaferCond fires) drains all
  // librbd async cleanup before we yank the rados connection out from under it.
  to_stop.clear();

  if (m_throttler) {
    dout(10) << "waiting for throttler operations to complete" << dendl;
    m_throttler->wait_for_ops();
    m_throttler.reset();
  }

  m_threads.reset();
  m_rados.shutdown();

  // Signal any concurrent caller (the other of signal-handler vs main) that
  // shutdown is fully complete and m_threads/m_throttler are reset, so it is
  // now safe to let ~BackfillDaemon run.
  {
    Mutex::Locker locker(m_lock);
    m_shutdown_done = true;
    m_shutdown_cond.SignalAll();
  }

  dout(5) << "daemon shutdown complete" << dendl;
}

int BackfillDaemon::connect_to_cluster() {
  dout(10) << dendl;

  int r = m_rados.init_with_context(m_cct);
  if (r < 0) {
    derr << "failed to initialize RADOS: " << cpp_strerror(r) << dendl;
    return r;
  }

  r = m_rados.connect();
  if (r < 0) {
    derr << "failed to connect to RADOS: " << cpp_strerror(r) << dendl;
    return r;
  }

  dout(10) << "connected to cluster successfully" << dendl;
  return 0;
}

int BackfillDaemon::discover_scheduled_images(std::vector<ImageSpec>* new_specs) {
  dout(10) << dendl;

  ceph_assert(new_specs != nullptr);

  // Snapshot the in-progress set so we can skip images we already claimed in
  // a prior pass.  Without this, a still-running backfill on image X would
  // end up appended again on the next rescan tick.  Copying under m_lock and
  // then iterating without it keeps the per-tick scan fast.
  std::set<ImageSpec> already_running;
  {
    Mutex::Locker locker(m_lock);
    for (auto& kv : m_image_backfillers) {
      already_running.insert(kv.first);
    }
  }

  // List all pools
  std::list<std::pair<int64_t, std::string>> pools;
  int r = m_rados.pool_list2(pools);
  if (r < 0) {
    derr << "failed to list pools: " << cpp_strerror(r) << dendl;
    return r;
  }

  dout(10) << "scanning " << pools.size() << " pools for scheduled images "
           << "(skipping " << already_running.size() << " already running)" << dendl;

  // Iterate through each pool to find scheduled images
  for (const auto& pool : pools) {
    std::string pool_name = pool.second;
    int64_t pool_id = pool.first;

    librados::IoCtx ioctx;
    r = m_rados.ioctx_create2(pool_id, ioctx);
    if (r < 0) {
      dout(10) << "skipping pool " << pool_name << " (cannot create ioctx): "
               << cpp_strerror(r) << dendl;
      continue;
    }

    // Collect namespaces: always include the default namespace ("") plus any
    // named namespaces so that scheduled images in non-default namespaces
    // are not silently missed.
    librbd::RBD rbd;
    std::vector<std::string> namespaces;
    r = rbd.namespace_list(ioctx, &namespaces);
    if (r < 0) {
      dout(10) << "failed to list namespaces in pool " << pool_name
               << ": " << cpp_strerror(r) << "; scanning default namespace only" << dendl;
    }
    // Always include the default (empty) namespace
    namespaces.push_back("");

    for (const auto& ns : namespaces) {
      ioctx.set_namespace(ns);

      std::vector<librbd::image_spec_t> images;
      r = rbd.list2(ioctx, &images);
      if (r < 0) {
        dout(10) << "failed to list images in pool " << pool_name
                 << (ns.empty() ? "" : "/" + ns)
                 << ": " << cpp_strerror(r) << dendl;
        continue;
      }

      // Phase 1: fire all metadata reads in parallel — one RADOS RTT regardless
      // of pool size.  Reading the full image context (rbd.open) for every image
      // just to check one metadata key is O(N) sequential RTTs; the async batch
      // collapses this to ~1 RTT before we open only the images that need claiming.
      struct PendingMeta {
        const librbd::image_spec_t* image_spec;
        librados::AioCompletion* c;
        librados::ObjectReadOperation op;
        bufferlist out_bl;
      };

      std::vector<PendingMeta> pending(images.size());
      for (size_t i = 0; i < images.size(); ++i) {
        pending[i].image_spec = &images[i];
        pending[i].c = librados::Rados::aio_create_completion();
        librbd::cls_client::metadata_list_start(&pending[i].op, BACKFILL_META_NS, 5);
        ioctx.aio_operate(librbd::util::header_name(images[i].id),
                          pending[i].c, &pending[i].op, &pending[i].out_bl);
      }

      // Phase 2: collect results; only open+claim images scheduled for backfill.
      for (auto& p : pending) {
        p.c->wait_for_complete();
        int op_r = p.c->get_return_value();
        p.c->release();
        if (op_r < 0) {
          continue;
        }

        std::map<std::string, bufferlist> pairs;
        auto it = p.out_bl.cbegin();
        if (librbd::cls_client::metadata_list_finish(&it, &pairs) < 0) {
          continue;
        }

        auto sched_it = pairs.find(BACKFILL_SCHEDULED_KEY);
        if (sched_it == pairs.end()) {
          continue;
        }
        std::string scheduled_value = sched_it->second.to_str();

        if (scheduled_value == BACKFILL_SCHED_IN_PROGRESS) {
          // Another daemon instance already claimed this image — skip it.
          dout(10) << "image " << pool_name
                   << (ns.empty() ? "" : "/" + ns)
                   << "/" << p.image_spec->name
                   << " is already claimed by another daemon instance, skipping" << dendl;
          continue;
        }

        if (scheduled_value != BACKFILL_SCHED_TRUE) {
          continue;
        }

        // Claim the image: open it and transition "true" → "in_progress" so
        // a second daemon instance starting concurrently sees "in_progress".
        // This is not a true CAS but the race window is negligible in practice.
        librbd::Image image;
        r = rbd.open(ioctx, image, p.image_spec->name.c_str());
        if (r < 0) {
          dout(10) << "failed to open image " << pool_name
                   << (ns.empty() ? "" : "/" + ns)
                   << "/" << p.image_spec->name << ": " << cpp_strerror(r) << dendl;
          continue;
        }

        int claim_r = image.metadata_set(BACKFILL_SCHEDULED_KEY, BACKFILL_SCHED_IN_PROGRESS);
        image.close();
        if (claim_r < 0) {
          dout(5) << "failed to claim image " << pool_name
                  << (ns.empty() ? "" : "/" + ns)
                  << "/" << p.image_spec->name
                  << " for backfill: " << cpp_strerror(claim_r)
                  << " — skipping to avoid duplicate backfill" << dendl;
          continue;
        }

        ImageSpec spec;
        spec.pool_name = pool_name;
        spec.pool_id = pool_id;
        spec.namespace_name = ns;
        spec.image_name = p.image_spec->name;
        spec.image_id = p.image_spec->id;

        if (already_running.count(spec)) {
          // Picked up in a prior tick; image transitioned to in_progress in
          // its own metadata but we already have a backfiller running.  Skip.
          continue;
        }

        new_specs->push_back(spec);

        dout(10) << "claimed and discovered scheduled image: " << pool_name
                 << (ns.empty() ? "" : "/" + ns)
                 << "/" << p.image_spec->name
                 << " (pool_id=" << pool_id << " image_id=" << spec.image_id
                 << ")" << dendl;
      }
    }
  }

  if (new_specs->empty()) {
    dout(10) << "no newly scheduled images found this pass" << dendl;
  } else {
    dout(5) << "discovered " << new_specs->size() << " new scheduled image(s)" << dendl;
  }

  return 0;
}

int BackfillDaemon::start_image_backfillers(const std::vector<ImageSpec>& specs) {
  dout(10) << "starting " << specs.size() << " backfiller(s)" << dendl;

  for (const auto& spec : specs) {
    dout(10) << "starting backfiller for " << spec.pool_name
             << "/" << spec.image_name << dendl;

    Context *on_finish = new FunctionContext([this, spec](int r) {
      handle_image_complete(spec, r);
    });

    auto backfiller = std::make_unique<ImageBackfiller>(
      m_cct,
      m_rados,
      spec,
      m_throttler.get(),
      m_threads.get(),
      on_finish
    );

    int r = backfiller->init();
    if (r < 0) {
      derr << "failed to initialize backfiller for " << spec.pool_name
           << "/" << spec.image_name << ": " << cpp_strerror(r) << dendl;
      delete on_finish;
      return r;
    }

    backfiller->create("img_backfill");
    {
      Mutex::Locker locker(m_lock);
      m_image_backfillers[spec] = std::move(backfiller);
    }

    dout(5) << "started backfiller for " << spec.pool_name
            << "/" << spec.image_name << dendl;
  }

  return 0;
}

// Caller must hold m_threads->timer_lock.  Splitting the locking responsibility
// from this function avoids a deadlock: SafeTimer is constructed with
// safe_callbacks=true, so the timer thread holds timer_lock while running our
// FunctionContext.  Re-acquiring timer_lock inside that callback would deadlock.
void BackfillDaemon::schedule_rescan() {
  if (m_rescan_interval == 0 || m_shutdown.load()) {
    return;
  }
  if (m_rescan_event != nullptr) {
    // Already armed.  Don't double-arm.
    return;
  }
  m_rescan_event = new FunctionContext([this](int r) {
    // Runs on the timer thread with timer_lock HELD (safe_callbacks=true).
    // Drop the lock around the heavy scan work (which may take seconds), then
    // re-acquire it just for the re-arm.  Without dropping, we'd hold up any
    // other timer events (and shutdown's cancel_event) for the whole scan.
    //
    // m_shutdown is monotonic (set once via exchange(true)), so a single
    // load before the work is sufficient — the re-arm guard below picks up
    // a flip during the scan because we re-load there too.
    m_rescan_event = nullptr;
    m_threads->timer_lock.Unlock();
    // try/catch is defensive: discover_scheduled_images / start_image_backfillers
    // are int-returning today, but anything they call deeper (RADOS, librbd
    // open) could throw bad_alloc.  Re-acquire timer_lock on rethrow so the
    // timer thread doesn't deadlock its own caller.
    try {
      if (!m_shutdown.load()) {
        rescan_tick();
      }
    } catch (...) {
      m_threads->timer_lock.Lock();
      throw;
    }
    m_threads->timer_lock.Lock();
    if (!m_shutdown.load()) {
      schedule_rescan();
    }
  });
  m_threads->timer->add_event_after(m_rescan_interval, m_rescan_event);
  dout(20) << "rescan timer armed for +" << m_rescan_interval << "s" << dendl;
}

// Must be called WITHOUT timer_lock held — pool_list2 / discover do RADOS
// ops that can take seconds.
void BackfillDaemon::rescan_tick() {
  std::vector<ImageSpec> new_specs;
  int r = discover_scheduled_images(&new_specs);
  if (r < 0) {
    derr << "rescan: discover failed: " << cpp_strerror(r)
         << "; will retry on next tick" << dendl;
    return;
  }
  if (new_specs.empty()) {
    return;
  }
  r = start_image_backfillers(new_specs);
  if (r < 0) {
    derr << "rescan: failed to start " << new_specs.size()
         << " new backfiller(s): " << cpp_strerror(r) << dendl;
    return;
  }
  dout(5) << "rescan: started " << new_specs.size()
          << " newly scheduled backfiller(s)" << dendl;
}

void BackfillDaemon::handle_image_complete(const ImageSpec& spec, int r) {
  dout(10) << "spec=" << spec.pool_name << "/" << spec.image_name
           << " r=" << r << dendl;

  Mutex::Locker locker(m_lock);

  auto it = m_image_backfillers.find(spec);
  if (it == m_image_backfillers.end()) {
    dout(5) << "image backfiller not found (already removed?)" << dendl;
    return;
  }

  dout(5) << "image backfill complete: " << spec.pool_name
          << "/" << spec.image_name << " r=" << r << dendl;

  m_image_backfillers.erase(it);

  // Auto-shutdown only in legacy one-shot mode.  When rescan is enabled, the
  // daemon stays alive between scheduling cycles — exiting on empty-map would
  // race with `rbd backfill schedule` invocations and force operators to
  // restart the daemon for every new image.
  if (m_rescan_interval == 0 && m_image_backfillers.empty()) {
    dout(5) << "all image backfillers complete (rescan disabled), initiating shutdown" << dendl;
    m_stopping = true;
    m_cond.Signal();
  }
}

} // namespace backfill
} // namespace rbd
