// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H
#define CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "include/Context.h"
#include <string>
#include <memory>

namespace librbd {
namespace io {

/**
 * S3ObjectFetcher - Fetches data from S3-backed parent images
 *
 * Supports HTTP Range requests to fetch specific byte ranges from
 * parent disk images stored in S3-compatible object storage.
 */
class S3ObjectFetcher {
public:
  struct S3Config {
    std::string endpoint;      // S3 endpoint URL (e.g., "http://127.0.0.1:9000")
    std::string bucket_name;   // S3 bucket name
    std::string object_key;    // S3 object key (image_name)
    std::string access_key;    // S3 access key
    std::string secret_key;    // S3 secret key
    std::string region;        // AWS region (e.g., "us-east-1")
    std::string image_format;  // Image format: "raw" or "qcow2"
    uint64_t object_size;      // RBD object size (for offset calculation)
  };

  S3ObjectFetcher(const S3Config& config);
  ~S3ObjectFetcher();

  /**
   * Fetch data from S3 parent image
   *
   * @param object_no RBD object number
   * @param object_off Offset within the object
   * @param length Number of bytes to read
   * @param out_bl Output buffer to fill with data
   * @param on_finish Completion callback (called with result code)
   */
  void fetch(uint64_t object_no, uint64_t object_off, uint64_t length,
             ceph::bufferlist* out_bl, Context* on_finish);

  /**
   * Synchronous fetch (for testing)
   */
  int fetch_sync(uint64_t object_no, uint64_t object_off, uint64_t length,
                 ceph::bufferlist* out_bl);

private:
  S3Config m_config;

  struct FetchContext {
    S3ObjectFetcher* fetcher;
    uint64_t offset;
    uint64_t length;
    ceph::bufferlist* out_bl;
    Context* on_finish;
    CURL* curl_handle;
    struct curl_slist* headers;
  };

  // Calculate byte offset in S3 object from RBD object number
  uint64_t calculate_s3_offset(uint64_t object_no, uint64_t object_off);

  // Perform HTTP Range GET request
  int do_http_range_get(uint64_t offset, uint64_t length, ceph::bufferlist* out_bl);

  // Generate AWS Signature V4 headers for a request
  void add_auth_headers(CURL* curl, struct curl_slist** headers,
                       const std::string& url, const std::string& range_header);

  // Extract host from URL
  std::string extract_host_from_url(const std::string& url);

  // Extract URI path from URL
  std::string extract_uri_from_url(const std::string& url);

  // Thread entry point for async fetch processing
  static void* async_fetch_thread(void* arg);
};

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H
