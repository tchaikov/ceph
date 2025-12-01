// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "BackfillDaemon.h"
#include "BackfillThrottler.h"
#include "ImageBackfiller.h"
#include "include/rados/librados.hpp"
#include "common/debug.h"
#include "common/errno.h"
#include "common/WorkQueue.h"
#include "common/Timer.h"

#define dout_context m_cct
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd::backfill::BackfillDaemon: " << this << " " << __func__ << ": "

namespace rbd {
namespace backfill {

// Threads implementation
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
  if (timer) {
    Mutex::Locker timer_locker(timer_lock);
    timer->shutdown();
  }

  // work_queue will be automatically deleted before thread_pool
  // due to unique_ptr destruction order

  if (thread_pool) {
    thread_pool->stop();
  }

  // Smart pointers handle cleanup automatically
}

// BackfillDaemon implementation
BackfillDaemon::BackfillDaemon(CephContext *cct,
                               const std::vector<ImageSpec>& images)
  : m_cct(cct),
    m_image_specs(images),
    m_lock("rbd::backfill::BackfillDaemon::m_lock") {
  dout(10) << "images=" << images.size() << dendl;
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

  r = resolve_image_specs();
  if (r < 0) {
    derr << "failed to resolve image specs: " << cpp_strerror(r) << dendl;
    return r;
  }

  // Initialize thread infrastructure
  m_threads = std::make_unique<Threads>(m_cct);

  // Initialize throttler
  m_throttler = std::make_unique<BackfillThrottler>(m_cct, m_threads->work_queue.get());

  dout(5) << "daemon initialized successfully" << dendl;
  return 0;
}

void BackfillDaemon::run() {
  dout(10) << dendl;

  int r = start_image_backfillers();
  if (r < 0) {
    derr << "failed to start image backfillers: " << cpp_strerror(r) << dendl;
    return;
  }

  dout(5) << "started " << m_image_backfillers.size() << " image backfillers" << dendl;

  // Main loop - wait for shutdown signal
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

  {
    Mutex::Locker locker(m_lock);
    m_stopping = true;
    m_cond.Signal();
  }

  // Stop all image backfillers
  for (auto& pair : m_image_backfillers) {
    dout(10) << "stopping backfiller for " << pair.first.pool_name
             << "/" << pair.first.image_name << dendl;
    pair.second->stop();
  }
  m_image_backfillers.clear();

  // Cleanup throttler
  if (m_throttler) {
    // Wait for all inflight operations to complete before destroying
    dout(10) << "waiting for throttler operations to complete" << dendl;
    m_throttler->wait_for_ops();
    m_throttler.reset();
  }

  // Cleanup threads
  m_threads.reset();

  // Disconnect from cluster
  m_rados.shutdown();

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

int BackfillDaemon::resolve_image_specs() {
  dout(10) << dendl;

  for (auto& spec : m_image_specs) {
    // Resolve pool name to pool ID
    librados::IoCtx ioctx;
    int r = m_rados.ioctx_create(spec.pool_name.c_str(), ioctx);
    if (r < 0) {
      derr << "failed to open pool " << spec.pool_name << ": "
           << cpp_strerror(r) << dendl;
      return r;
    }

    spec.pool_id = ioctx.get_id();

    // TODO: Resolve image name to image ID
    // For now, just use image name as image ID (will need librbd API)
    spec.image_id = spec.image_name;

    dout(10) << "resolved " << spec.pool_name << "/" << spec.image_name
             << " to pool_id=" << spec.pool_id << " image_id=" << spec.image_id
             << dendl;
  }

  return 0;
}

int BackfillDaemon::start_image_backfillers() {
  dout(10) << dendl;

  for (const auto& spec : m_image_specs) {
    dout(10) << "starting backfiller for " << spec.pool_name
             << "/" << spec.image_name << dendl;

    // Create completion callback
    Context *on_finish = new FunctionContext([this, spec](int r) {
      handle_image_complete(spec, r);
    });

    // Create ImageBackfiller instance using unique_ptr
    auto backfiller = std::make_unique<ImageBackfiller>(
      m_cct,
      m_rados,
      spec,
      m_throttler.get(),
      m_threads.get(),
      on_finish
    );

    // Initialize the backfiller
    int r = backfiller->init();
    if (r < 0) {
      derr << "failed to initialize backfiller for " << spec.pool_name
           << "/" << spec.image_name << ": " << cpp_strerror(r) << dendl;
      delete on_finish;
      return r;
    }

    // Start the backfill thread
    backfiller->create("img_backfill");

    // Track the backfiller
    m_image_backfillers[spec] = std::move(backfiller);

    dout(5) << "started backfiller for " << spec.pool_name
            << "/" << spec.image_name << dendl;
  }

  return 0;
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

  // Smart pointer will automatically clean up when erased
  m_image_backfillers.erase(it);

  // If all backfillers are done, we can shut down
  if (m_image_backfillers.empty()) {
    dout(5) << "all image backfillers complete, initiating shutdown" << dendl;
    m_stopping = true;
    m_cond.Signal();
  }
}

} // namespace backfill
} // namespace rbd
