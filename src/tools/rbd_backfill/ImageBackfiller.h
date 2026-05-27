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

  // True iff run_backfill has finished its object-copy loop AND closed
  // the open ImageCtx (releasing the RADOS watcher on the header).  The
  // backfiller thread is still alive (idling for stop()), but the daemon
  // can safely prune it from m_image_backfillers and join+destroy it on
  // the next rescan tick.  Required for the cancel-after-completion
  // scenario: without proactive image close, `rbd rm base` after
  // `rbd backfill cancel base` fails because the daemon's watcher
  // outlives the backfill work.
  bool is_done() const { return m_done.load(); }

private:
  void run_backfill();
  void backfill_object(uint64_t object_no);
  void handle_object_complete(int r);
  void load_s3_config();

  // Idempotent create+size of the per-image backfill-visited bitmap
  // (rbd_backfill_visited.<image_id>).  Non-fatal on failure: the bitmap
  // is an optimization for the reader's ENOENT short-circuit; backfill
  // proceeds without it.
  void init_backfill_visited_bitmap();

  CephContext *m_cct;
  librados::Rados& m_rados;
  ImageSpec m_spec;
  BackfillThrottler *m_throttler;
  Threads *m_threads;
  Context *m_on_finish;

  librados::IoCtx m_ioctx;
  std::unique_ptr<librbd::ImageCtx> m_image_ctx;
  std::unique_ptr<librbd::io::S3ObjectFetcher> m_s3_fetcher;

  std::atomic<bool> m_stopping{false};
  std::atomic<bool> m_done{false};
  Mutex m_lock;
  Cond m_cond;

  uint64_t m_num_objects = 0;
  std::atomic<uint64_t> m_completed_objects{0};
  std::atomic<uint64_t> m_failed_objects{0};

  // Close the open ImageCtx and unregister the RADOS watcher.  Idempotent --
  // both proactive (post-backfill) close and the destructor call this; the
  // second call is a no-op because release()-then-close also nulls the
  // unique_ptr's stored pointer.
  void close_image_if_open();

};

} // namespace backfill
} // namespace rbd

#endif // CEPH_RBD_BACKFILL_IMAGE_BACKFILLER_H
