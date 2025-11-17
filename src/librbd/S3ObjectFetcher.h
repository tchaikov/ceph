// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_S3_OBJECT_FETCHER_H
#define CEPH_LIBRBD_S3_OBJECT_FETCHER_H

#include "include/buffer.h"
#include "include/Context.h"
#include <string>

typedef void CURL;

class CephContext;

namespace librbd {

/**
 * S3ObjectFetcher - Simple HTTP client for fetching objects from S3
 *
 * This class provides asynchronous object fetching from S3 storage using libcurl.
 * It supports anonymous S3 access (no authentication) for use cases where objects
 * are publicly accessible.
 *
 * Key features:
 * - Anonymous S3 access (simple HTTP GET)
 * - Timeout and retry handling
 * - Async operation (planned for future)
 * - libcurl-based (reuses Ceph's existing dependency)
 */
class S3ObjectFetcher {
public:
  /**
   * Construct S3ObjectFetcher
   *
   * @param cct Ceph context for logging and configuration
   */
  explicit S3ObjectFetcher(CephContext* cct);

  ~S3ObjectFetcher();

  /**
   * Fetch object from S3 via HTTP GET with optional range
   *
   * Performs an HTTP GET request to fetch an object from S3 storage.
   * Supports HTTP range requests for fetching partial objects.
   * Currently synchronous, will be made async in future optimization.
   *
   * @param url Full S3 URL (e.g., "https://bucket.s3.amazonaws.com/key")
   * @param data Output buffer to store fetched data
   * @param on_finish Completion callback (called with return code)
   * @param byte_start Start byte offset for range request (0 = beginning)
   * @param byte_length Number of bytes to fetch (0 = fetch entire object)
   */
  void fetch(
    const std::string& url,
    bufferlist* data,
    Context* on_finish,
    uint64_t byte_start = 0,
    uint64_t byte_length = 0);

private:
  CephContext* m_cct;

  /**
   * Setup curl handle for HTTP GET request with optional range
   *
   * @param url Target URL
   * @param data Output buffer for response data
   * @param byte_start Start byte offset for range request
   * @param byte_length Number of bytes to fetch
   * @return Configured CURL handle
   */
  CURL* setup_curl_handle(const std::string& url, bufferlist* data,
                          uint64_t byte_start, uint64_t byte_length);

  /**
   * Perform HTTP GET with retry logic
   *
   * @param url Target URL
   * @param data Output buffer
   * @param max_retries Maximum number of retry attempts
   * @param byte_start Start byte offset for range request
   * @param byte_length Number of bytes to fetch
   * @return 0 on success, negative error code on failure
   */
  int fetch_with_retry(
    const std::string& url,
    bufferlist* data,
    uint32_t max_retries,
    uint64_t byte_start,
    uint64_t byte_length);

  /**
   * libcurl write callback for response data
   *
   * Called by libcurl when response data is received.
   *
   * @param ptr Pointer to received data
   * @param size Size of each data element
   * @param nmemb Number of data elements
   * @param userdata User-provided context (bufferlist*)
   * @return Number of bytes processed
   */
  static size_t write_callback(
    void* ptr,
    size_t size,
    size_t nmemb,
    void* userdata);
};

} // namespace librbd

#endif // CEPH_LIBRBD_S3_OBJECT_FETCHER_H
