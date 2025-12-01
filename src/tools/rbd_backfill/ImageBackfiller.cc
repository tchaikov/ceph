// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ImageBackfiller.h"
#include "BackfillDaemon.h"
#include "BackfillThrottler.h"
#include "ObjectBackfillRequest.h"
#include "include/rados/librados.hpp"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/Types.h"
#include "librbd/internal.h"
#include "librbd/io/S3ObjectFetcher.h"
#include "common/debug.h"
#include "common/errno.h"

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
    m_lock("ImageBackfiller::m_lock") {
  dout(10) << dendl;
}

ImageBackfiller::~ImageBackfiller() {
  dout(10) << dendl;

  // unique_ptr will automatically delete m_s3_fetcher

  if (m_image_ctx) {
    m_image_ctx->state->close();
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

  // Open the parent image
  m_image_ctx.reset(new librbd::ImageCtx(m_spec.image_name, "", "", m_ioctx, false));

  r = m_image_ctx->state->open(0);
  if (r < 0) {
    derr << "failed to open image " << m_spec.pool_name << "/"
         << m_spec.image_name << ": " << cpp_strerror(r) << dendl;
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

  dout(5) << "initial backfill complete: completed=" << m_completed_objects.load()
          << " failed=" << m_failed_objects.load()
          << " total=" << m_num_objects << dendl;

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

  // Wait for throttler to allow this operation
  C_SaferCond wait_ctx;
  m_throttler->start_op(object_no, &wait_ctx);

  int r = wait_ctx.wait();
  if (r < 0) {
    derr << "throttler failed for object " << object_no << ": "
         << cpp_strerror(r) << dendl;
    m_failed_objects++;
    return;
  }

  if (m_stopping.load()) {
    dout(15) << "stopping requested, aborting object_no=" << object_no << dendl;
    m_throttler->finish_op(object_no);
    return;
  }

  // Check if S3 fetcher is available
  if (!m_s3_fetcher) {
    derr << "S3 fetcher not configured for object " << object_no << dendl;
    m_failed_objects++;
    m_throttler->finish_op(object_no);
    return;
  }

  // Fetch data from S3 synchronously (we're in ImageBackfiller thread, blocking is OK)
  bufferlist data_bl;
  uint64_t object_size = 1ull << m_image_ctx->order;

  dout(20) << "fetching from S3: object_no=" << object_no
           << " size=" << object_size << dendl;
  r = m_s3_fetcher->fetch_sync(object_no, 0, object_size, &data_bl);

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

  if (data_bl.length() != expected_size) {
    derr << "S3 returned unexpected size for object " << object_no
         << ": expected=" << expected_size << " got=" << data_bl.length() << dendl;
    m_failed_objects++;
    m_throttler->finish_op(object_no);
    return;
  }

  // Construct object name using ImageCtx's format_string (hex-formatted with zero-padding)
  char object_name_buf[RBD_MAX_OBJ_NAME_SIZE];
  snprintf(object_name_buf, sizeof(object_name_buf), m_image_ctx->format_string, object_no);
  std::string object_name(object_name_buf);

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
    data_bl,  // Pass pre-fetched data
    m_cct,
    m_threads,
    on_complete
  );

  dout(20) << "sending ObjectBackfillRequest for object_no=" << object_no << dendl;
  req->send();
}

void ImageBackfiller::handle_object_complete(int r) {
  if (r < 0) {
    dout(10) << "object backfill failed: " << cpp_strerror(r) << dendl;
    m_failed_objects++;
    m_ret_val = r;
  } else {
    dout(20) << "object backfill succeeded" << dendl;
    m_completed_objects++;
  }

  m_current_object++;

  dout(15) << "progress: " << m_current_object << "/" << m_num_objects
           << " (completed=" << m_completed_objects.load()
           << " failed=" << m_failed_objects.load() << ")" << dendl;
}

void ImageBackfiller::load_s3_config() {
  dout(10) << dendl;

  // Load S3 configuration from image metadata
  // Metadata keys: s3_endpoint, s3_bucket_name, s3_object_key,
  //                s3_access_key, s3_secret_key, s3_region, s3_image_format

  librbd::S3Config s3_config;

  // Helper lambda to get metadata value
  auto get_metadata = [this](const std::string& key, std::string& value) -> bool {
    int r = librbd::metadata_get(m_image_ctx.get(), key, &value);
    if (r < 0 && r != -ENOENT) {
      dout(5) << "warning: failed to read " << key << ": "
              << cpp_strerror(r) << dendl;
      return false;
    }
    return (r == 0);
  };

  // Check if S3 is enabled
  std::string endpoint, bucket, image_name;
  if (!get_metadata("s3.endpoint", endpoint)) {
    dout(10) << "S3 not configured for image (no s3.endpoint metadata)" << dendl;
    return;
  }

  if (!get_metadata("s3.bucket", bucket)) {
    dout(5) << "S3 enabled but missing s3.bucket metadata" << dendl;
    return;
  }

  if (!get_metadata("s3.image_name", image_name)) {
    dout(5) << "S3 enabled but missing s3.image_name metadata" << dendl;
    return;
  }

  // Populate S3Config
  s3_config.enabled = true;
  s3_config.endpoint = endpoint;
  s3_config.bucket = bucket;
  s3_config.image_name = image_name;

  // Get optional fields
  get_metadata("s3.region", s3_config.region);
  get_metadata("s3.access_key", s3_config.access_key);
  get_metadata("s3.prefix", s3_config.prefix);

  // Secret key is stored base64-encoded
  std::string encoded_secret_key;
  if (get_metadata("s3.secret_key", encoded_secret_key)) {
    bufferlist encoded_bl;
    encoded_bl.append(encoded_secret_key);
    bufferlist decoded_bl;
    try {
      decoded_bl.decode_base64(encoded_bl);
      s3_config.secret_key = decoded_bl.to_str();
    } catch (buffer::error& err) {
      // Base64 decode failed - try using raw value (for testing/MinIO)
      dout(10) << "note: s3_secret_key not base64-encoded, using raw value" << dendl;
      s3_config.secret_key = encoded_secret_key;
    }
  }

  get_metadata("s3.image_format", s3_config.image_format);

  // Set object_size - THIS IS CRITICAL for offset calculation
  s3_config.object_size = 1ull << m_image_ctx->order;

  // Validate configuration
  if (!s3_config.is_valid()) {
    derr << "S3 configuration is invalid!" << dendl;
    return;
  }

  dout(10) << "loaded S3 configuration: "
           << "endpoint=" << s3_config.endpoint
           << ", bucket=" << s3_config.bucket
           << ", image_name=" << s3_config.image_name
           << ", object_size=" << s3_config.object_size
           << ", format=" << s3_config.image_format << dendl;

  // Create S3ObjectFetcher using unique_ptr
  m_s3_fetcher = std::make_unique<librbd::io::S3ObjectFetcher>(m_cct, s3_config);
  dout(10) << "created S3ObjectFetcher successfully" << dendl;
}

} // namespace backfill
} // namespace rbd
