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

class CephContext;
class ContextWQ;
class SafeTimer;
class ThreadPool;

namespace rbd {
namespace backfill {

class BackfillThrottler;
class ImageBackfiller;

struct Threads {
  ThreadPool *thread_pool = nullptr;
  ContextWQ *work_queue = nullptr;
  SafeTimer *timer = nullptr;
  Mutex timer_lock;

  explicit Threads(CephContext *cct);
  ~Threads();

  Threads(const Threads&) = delete;
  Threads& operator=(const Threads&) = delete;
};

class BackfillDaemon {
public:
  BackfillDaemon(CephContext *cct, const std::vector<ImageSpec>& images);
  ~BackfillDaemon();

  BackfillDaemon(const BackfillDaemon&) = delete;
  BackfillDaemon& operator=(const BackfillDaemon&) = delete;

  int init();
  void run();
  void shutdown();

private:
  int connect_to_cluster();
  int resolve_image_specs();
  int start_image_backfillers();

  void handle_image_complete(const ImageSpec& spec, int r);

  CephContext *m_cct;
  Threads *m_threads = nullptr;
  BackfillThrottler *m_throttler = nullptr;

  librados::Rados m_rados;

  std::vector<ImageSpec> m_image_specs;
  std::map<ImageSpec, ImageBackfiller*> m_image_backfillers;

  Mutex m_lock;
  Cond m_cond;
  bool m_stopping = false;
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_RBD_BACKFILL_DAEMON_H
