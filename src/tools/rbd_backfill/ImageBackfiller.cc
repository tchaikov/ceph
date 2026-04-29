// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ImageBackfiller.h"
#include "Types.h"
#include "BackfillDaemon.h"
#include "BackfillThrottler.h"
#include "ObjectBackfillRequest.h"
#include "include/rados/librados.hpp"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/Types.h"
#include "librbd/io/S3ObjectFetcher.h"
#include "cls/rbd/cls_rbd_client.h"
#include "common/debug.h"
#include "common/errno.h"
#include "librbd/Utils.h"

#define dout_context m_cct
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd::backfill::ImageBackfiller: " \
                           << m_spec.pool_name << "/" << m_spec.image_name \
                           << " " << __func__ << ": "

namespace rbd {
namespace backfill {

ImageBackfiller::ImageBackfiller(CephContext *cct,
                                 librados::Rados& rados,
                                 const ImageSpec& spec,
                                 BackfillThrottler *throttler,
                                 Threads *threads,
                                 Context *on_finish)
  : m_cct(cct),
    m_rados(rados),
    m_spec(spec),
    m_throttler(throttler),
    m_threads(threads),
    m_on_finish(on_finish),
    m_lock(librbd::util::unique_lock_name("ImageBackfiller::m_lock", this)) {
  dout(10) << dendl;
}

ImageBackfiller::~ImageBackfiller() {
  dout(10) << dendl;

  if (m_image_ctx) {
    // librbd::ImageState<I>::close() takes ownership of the ImageCtx and
    // deletes it via `delete m_image_ctx` after the close completes (see
    // src/librbd/ImageState.cc:279).  Release the unique_ptr first so it
    // doesn't run a second `delete` on the now-freed ImageCtx — the second
    // delete races against any heap-reuser, eventually segfaulting in
    // perf_stop() when cct gets zeroed by an unrelated allocator op.
    librbd::ImageCtx* ictx = m_image_ctx.release();
    ictx->state->close();
  }
}

int ImageBackfiller::init() {
  dout(10) << dendl;

  // Create IoCtx for the pool
  int r = m_rados.ioctx_create(m_spec.pool_name.c_str(), m_ioctx);
  if (r < 0) {
    derr << "failed to create IoCtx for pool " << m_spec.pool_name
         << ": " << cpp_strerror(r) << dendl;
    return r;
  }

  // Set namespace if specified
  if (!m_spec.namespace_name.empty()) {
    m_ioctx.set_namespace(m_spec.namespace_name);
  }

  // Open the parent image
  m_image_ctx.reset(new librbd::ImageCtx(m_spec.image_name, "", "", m_ioctx, false));

  r = m_image_ctx->state->open(0);
  if (r < 0) {
    derr << "failed to open image "
         << format_image_path(m_spec.pool_name, m_spec.namespace_name,
                              m_spec.image_name)
         << ": " << cpp_strerror(r) << dendl;
    m_image_ctx.reset();
    return r;
  }

  // Calculate number of objects
  uint64_t object_size = 1ull << m_image_ctx->order;
  m_num_objects = (m_image_ctx->size + object_size - 1) / object_size;

  dout(5) << "image opened: size=" << m_image_ctx->size
          << " object_size=" << object_size
          << " num_objects=" << m_num_objects << dendl;

  if (m_num_objects == 0) {
    dout(5) << "image is empty, nothing to backfill" << dendl;
    return 0;
  }

  // Load S3 configuration from image metadata
  load_s3_config();

  return 0;
}

void ImageBackfiller::stop() {
  dout(10) << dendl;

  {
    Mutex::Locker locker(m_lock);
    if (m_stopping.load()) {
      return;
    }
    m_stopping.store(true);
    m_cond.Signal();
  }

  // Wait for thread to complete
  if (is_started()) {
    join();
  }
}

void *ImageBackfiller::entry() {
  dout(5) << "backfill thread starting" << dendl;
  run_backfill();
  dout(5) << "backfill thread exiting" << dendl;
  return nullptr;
}

void ImageBackfiller::run_backfill() {
  dout(10) << "starting backfill: num_objects=" << m_num_objects << dendl;

  for (uint64_t obj_no = 0; obj_no < m_num_objects; ++obj_no) {
    if (m_stopping.load()) {
      dout(10) << "stopping requested at object " << obj_no << dendl;
      break;
    }

    backfill_object(obj_no);
  }

  dout(10) << "backfill loop complete, waiting for in-flight operations" << dendl;

  // Wait for all in-flight RADOS operations to complete
  // The throttler tracks all ObjectBackfillRequests that haven't finished yet
  m_throttler->wait_for_ops();

  uint64_t completed = m_completed_objects.load();
  uint64_t failed    = m_failed_objects.load();
  dout(5) << "initial backfill complete: completed=" << completed
          << " failed=" << failed
          << " total=" << m_num_objects << dendl;

  // Clean up sentinel lock objects (<oid>.s3lk) created during backfill.
  // These objects hold no data — they are purely the cls_lock target — but
  // they accumulate in RADOS (one per parent object) and inflate object counts,
  // affecting PG balancing and scrub times.  Remove them after all writes are
  // confirmed so that a restart (which now checks RADOS existence via stat)
  // still works correctly: the actual data objects are unaffected.
  if (completed + failed == m_num_objects && failed == 0) {
    dout(5) << "cleaning up " << m_num_objects
            << " sentinel lock objects (.s3lk)" << dendl;

    // Fire all removes in parallel and wait for the batch to finish.
    std::vector<librados::AioCompletion *> aios;
    aios.reserve(m_num_objects);
    for (uint64_t obj_no = 0; obj_no < m_num_objects; ++obj_no) {
      std::string sentinel_oid =
          m_image_ctx->get_object_name(obj_no) + librbd::S3_FETCH_LOCK_SENTINEL_SUFFIX;

      auto *c = librados::Rados::aio_create_completion(nullptr, nullptr, nullptr);
      if (m_ioctx.aio_remove(sentinel_oid, c) < 0) {
        c->release();
      } else {
        aios.push_back(c);
      }
    }
    for (auto *c : aios) {
      c->wait_for_complete();
      int rm_r = c->get_return_value();
      c->release();
      if (rm_r < 0 && rm_r != -ENOENT) {
        dout(10) << "failed to remove sentinel lock object: "
                 << cpp_strerror(rm_r) << dendl;
      }
    }
    dout(5) << "sentinel cleanup complete" << dendl;
  }

  // Update metadata so the daemon does not re-backfill this image on restart.
  // Clear backfill_scheduled (daemon discovery key) and record final status.
  // Use the synchronous cls_rbd path — we are in the ImageBackfiller thread
  // (not a RADOS completion callback), so blocking is acceptable.
  //
  // Use (completed + failed == m_num_objects) rather than !m_stopping: the
  // stop signal may arrive after the backfill loop finishes (e.g. the test
  // harness kills the daemon immediately after wait_for_backfill_complete),
  // in which case m_stopping is true but all work was done and the metadata
  // must be updated so the daemon doesn't re-queue the image on restart.
  if (completed + failed == m_num_objects) {
    std::string final_status = (failed == 0) ? BACKFILL_STATUS_COMPLETE
                                              : BACKFILL_STATUS_FAILED;

    // Write status FIRST so a crash between these two operations leaves the
    // image in a recoverable state: if status is set but scheduled is not yet
    // cleared, a restarted daemon will re-queue the image, stat each object
    // (finding them already written), and update status to "complete" again.
    // The reverse order (clear scheduled first) would leave status permanently
    // stale with no re-queue trigger.
    std::map<std::string, bufferlist> pairs;
    bufferlist bl;
    bl.append(final_status);
    pairs[BACKFILL_STATUS_KEY] = bl;
    int mr = librbd::cls_client::metadata_set(
               &m_ioctx, m_image_ctx->header_oid, pairs);
    if (mr < 0) {
      dout(5) << "warning: failed to update backfill_status: "
              << cpp_strerror(mr) << dendl;
    } else {
      dout(5) << "backfill_status updated to '" << final_status << "'" << dendl;
    }

    // Remove the scheduling flag so a restarted daemon does not re-queue
    // this image.  Ignore ENOENT in case it was already cleared externally.
    mr = librbd::cls_client::metadata_remove(
           &m_ioctx, m_image_ctx->header_oid, BACKFILL_SCHEDULED_KEY);
    if (mr < 0 && mr != -ENOENT) {
      dout(5) << "warning: failed to remove backfill_scheduled: "
              << cpp_strerror(mr) << dendl;
    }
  }

  // Keep daemon running - enter idle state waiting for shutdown signal
  // Don't call m_on_finish->complete() - that would trigger daemon shutdown
  dout(5) << "entering idle state" << dendl;

  {
    Mutex::Locker locker(m_lock);
    while (!m_stopping.load()) {
      dout(20) << "waiting for shutdown signal" << dendl;
      m_cond.Wait(m_lock);
    }
  }

  dout(5) << "shutdown signal received, exiting" << dendl;

  // Now we can complete with result
  if (m_on_finish != nullptr) {
    int result = (m_failed_objects.load() > 0) ? -EIO : 0;
    m_on_finish->complete(result);
  }
}

void ImageBackfiller::backfill_object(uint64_t object_no) {
  dout(15) << "object_no=" << object_no << dendl;

  // Pre-flight checks before touching any shared resources.
  if (m_stopping.load()) {
    dout(15) << "stopping requested, skipping object_no=" << object_no << dendl;
    return;
  }

  if (!m_s3_fetcher) {
    derr << "S3 fetcher not configured for object " << object_no << dendl;
    m_failed_objects++;
    return;
  }

  // Skip objects already in RADOS (efficient restart recovery).
  uint64_t psize = 0;
  time_t pmtime = 0;
  int stat_r = m_ioctx.stat(m_image_ctx->get_object_name(object_no), &psize, &pmtime);
  if (stat_r == 0) {
    dout(15) << "object " << object_no << " already in RADOS (size=" << psize
             << "), skipping S3 fetch" << dendl;
    m_completed_objects++;
    return;
  }
  if (stat_r != -ENOENT) {
    dout(5) << "stat for object " << object_no << " returned " << cpp_strerror(stat_r)
            << ", proceeding with S3 fetch anyway" << dendl;
  }

  uint64_t object_size = 1ull << m_image_ctx->order;

  // PIPELINE: kick off the S3 fetch NOW, before waiting for a RADOS write slot.
  //
  // The backfill loop submits RADOS writes asynchronously (via ObjectBackfillRequest)
  // and controls concurrency with BackfillThrottler.  When all slots are busy the
  // throttle wait below blocks — that blocking window is exactly where we want
  // the S3 download for THIS object to make progress instead of sitting idle.
  //
  // The async_fetch_thread uses the CURLSH* connection pool in m_s3_fetcher, so
  // after the first fetch the TCP/TLS connection to S3 is kept alive and reused.
  //
  // Safety: s3_ctx is on the stack; backfill_object() always reaches s3_ctx.wait()
  // before returning, so the C_SaferCond is alive for the full lifetime of the
  // detached pthread that signals it.
  bufferlist data_bl;
  C_SaferCond s3_ctx;
  dout(20) << "starting async S3 fetch: object_no=" << object_no
           << " size=" << object_size << dendl;
  m_s3_fetcher->fetch(object_no, 0, object_size, &data_bl, &s3_ctx);

  // Acquire a RADOS write slot.  This may block while previous writes drain —
  // the window where the S3 fetch above runs for free.
  C_SaferCond throttle_ctx;
  m_throttler->start_op(object_no, &throttle_ctx);
  int r = throttle_ctx.wait();
  if (r < 0) {
    derr << "throttler failed for object " << object_no << ": "
         << cpp_strerror(r) << dendl;
    s3_ctx.wait();  // must drain before stack unwinds
    m_failed_objects++;
    return;
  }

  // Stopping check: re-check after the (potentially long) throttle wait.
  if (m_stopping.load()) {
    dout(15) << "stopping requested after throttle wait, aborting object_no="
             << object_no << dendl;
    s3_ctx.wait();  // must drain before stack unwinds
    m_throttler->finish_op(object_no);
    return;
  }

  // Wait for the S3 fetch — if S3 was faster than the throttle wait it's
  // already done and s3_ctx.wait() returns immediately.
  r = s3_ctx.wait();
  if (r < 0) {
    derr << "S3 fetch failed for object " << object_no << ": "
         << cpp_strerror(r) << dendl;
    m_failed_objects++;
    m_throttler->finish_op(object_no);
    return;
  }

  if (m_stopping.load()) {
    dout(15) << "stopping requested after S3 fetch, aborting object_no="
             << object_no << dendl;
    m_throttler->finish_op(object_no);
    return;
  }

  // Validate data length - last object may be partial
  uint64_t expected_size = object_size;
  if (object_no == m_num_objects - 1) {
    // Last object size = image_size - (object_no * object_size)
    uint64_t last_object_size = m_image_ctx->size - (object_no * object_size);
    expected_size = last_object_size;
  }

  if (data_bl.length() > expected_size) {
    // S3 returned more data than expected — this should not happen with a
    // well-formed Range GET, but treat it as an error to avoid silent corruption.
    derr << "S3 returned too much data for object " << object_no
         << ": expected=" << expected_size << " got=" << data_bl.length() << dendl;
    m_failed_objects++;
    m_throttler->finish_op(object_no);
    return;
  }
  if (data_bl.length() < expected_size) {
    if (object_no < m_num_objects - 1) {
      // Short read on a non-last object means the S3 HTTP response was
      // truncated or the stored file is smaller than the image.  Do not
      // silently zero-pad: write the wrong data once and it is very hard
      // to detect.  Fail loudly instead.
      derr << "S3 returned short data for non-last object " << object_no
           << ": expected=" << expected_size << " got=" << data_bl.length()
           << " — aborting (S3 file may be truncated or wrong image size)" << dendl;
      m_failed_objects++;
      m_throttler->finish_op(object_no);
      return;
    }
    // Last object: the S3 file may not fill the final RBD object slot
    // (image size is not a multiple of object_size).  Zero-pad to the full
    // object size so the written RADOS object has the canonical length.
    dout(15) << "padding last object " << object_no
             << " from " << data_bl.length() << " to " << expected_size << dendl;
    data_bl.append_zero(expected_size - data_bl.length());
  }

  std::string object_name = m_image_ctx->get_object_name(object_no);

  // Create completion callback
  Context *on_complete = new FunctionContext([this, object_no](int r) {
    handle_object_complete(r);
    m_throttler->finish_op(object_no);
  });

  // Create and send backfill request with pre-fetched data
  // ObjectBackfillRequest handles RADOS write + lock management
  ObjectBackfillRequest *req = new ObjectBackfillRequest(
    m_image_ctx->data_ctx,
    object_name,
    object_no,
    data_bl,          // Pass pre-fetched data
    m_image_ctx->id,  // Image ID for object map updates
    m_cct,
    on_complete
  );

  dout(20) << "sending ObjectBackfillRequest for object_no=" << object_no << dendl;
  req->send();
}

void ImageBackfiller::handle_object_complete(int r) {
  if (r < 0) {
    dout(10) << "object backfill failed: " << cpp_strerror(r) << dendl;
    m_failed_objects++;
  } else {
    dout(20) << "object backfill succeeded" << dendl;
    m_completed_objects++;
  }

  dout(15) << "progress: "
           << (m_completed_objects.load() + m_failed_objects.load())
           << "/" << m_num_objects
           << " (completed=" << m_completed_objects.load()
           << " failed=" << m_failed_objects.load() << ")" << dendl;
}

void ImageBackfiller::load_s3_config() {
  dout(10) << dendl;

  // S3 config is already populated by ImageCtx::apply_metadata() during open().
  // Just validate and create the fetcher.
  const librbd::S3Config& s3_config = m_image_ctx->s3_config;

  if (!s3_config.is_valid()) {
    derr << "S3 not configured or invalid for image " << m_spec.image_name
         << " — check s3.bucket, s3.endpoint, s3.image_name, s3.image_format metadata"
         << dendl;
    return;
  }

  dout(10) << "S3 configuration: "
           << "endpoint=" << s3_config.endpoint
           << ", bucket=" << s3_config.bucket
           << ", image_name=" << s3_config.image_name
           << ", object_size=" << (1ull << m_image_ctx->order)
           << ", format=" << s3_config.image_format << dendl;

  m_s3_fetcher = std::make_unique<librbd::io::S3ObjectFetcher>(m_cct, s3_config, 1ull << m_image_ctx->order);
  dout(10) << "created S3ObjectFetcher" << dendl;
}

} // namespace backfill
} // namespace rbd
