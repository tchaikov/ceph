// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_BACKFILL_IMAGE_BACKFILLER_H
#define CEPH_RBD_BACKFILL_IMAGE_BACKFILLER_H

#include "Types.h"
#include "include/rados/librados.hpp"
#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/Thread.h"
#include <string>
#include <atomic>
#include <memory>

namespace librbd {
struct ImageCtx;
}

namespace librbd {
namespace io {
class S3ObjectFetcher;
}
}

namespace rbd {
namespace backfill {

class BackfillThrottler;
class Threads;

class ImageBackfiller : public Thread {
public:
  ImageBackfiller(CephContext *cct,
                  librados::Rados& rados,
                  const ImageSpec& spec,
                  BackfillThrottler *throttler,
                  Threads *threads,
                  Context *on_finish);

  ~ImageBackfiller() override;

  int init();
  void stop();

  // Thread interface
  void *entry() override;

private:
  void run_backfill();
  void backfill_object(uint64_t object_no);
  void handle_object_complete(int r);
  void load_s3_config();

  CephContext *m_cct;
  librados::Rados& m_rados;
  ImageSpec m_spec;
  BackfillThrottler *m_throttler;
  Threads *m_threads;
  Context *m_on_finish;

  librados::IoCtx m_ioctx;
  librbd::ImageCtx *m_image_ctx = nullptr;
  std::unique_ptr<librbd::io::S3ObjectFetcher> m_s3_fetcher;

  std::atomic<bool> m_stopping{false};
  Mutex m_lock;
  Cond m_cond;

  uint64_t m_num_objects = 0;
  uint64_t m_current_object = 0;
  std::atomic<uint64_t> m_completed_objects{0};
  std::atomic<uint64_t> m_failed_objects{0};

  int m_ret_val = 0;
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_RBD_BACKFILL_IMAGE_BACKFILLER_H
