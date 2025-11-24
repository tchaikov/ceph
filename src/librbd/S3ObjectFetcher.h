// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_S3_OBJECT_FETCHER_H
#define CEPH_LIBRBD_S3_OBJECT_FETCHER_H

#include "include/buffer.h"
#include "include/Context.h"
#include "librbd/Types.h"
#include <string>

typedef void CURL;
struct curl_slist;

class CephContext;

namespace librbd {

/**
 * S3ObjectFetcher - HTTP client for fetching objects from S3 with AWS Signature V4 auth
 *
 * This class provides object fetching from S3 storage using libcurl.
 * It supports AWS Signature V4 authentication for secure access.
 *
 * Key features:
 * - AWS Signature V4 authentication
 * - Anonymous S3 access (when no credentials provided)
 * - Timeout and retry handling
 * - libcurl-based (reuses Ceph's existing dependency)
 */
class S3ObjectFetcher {
public:
  /**
   * Construct S3ObjectFetcher with S3 configuration
   *
   * @param cct Ceph context for logging and configuration
   * @param s3_config S3 configuration including credentials
   */
  S3ObjectFetcher(CephContext* cct, const S3Config& s3_config);

  ~S3ObjectFetcher();

  /**
   * Fetch object from S3 via HTTP GET with optional range
   *
   * Performs an HTTP GET request to fetch an object from S3 storage.
   * Supports HTTP range requests for fetching partial objects.
   * Uses AWS Signature V4 authentication if credentials are provided.
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
  S3Config m_s3_config;

  /**
   * Extract host from URL (including port if present)
   */
  std::string extract_host_from_url(const std::string& url);

  /**
   * Extract URI path from URL
   */
  std::string extract_uri_from_url(const std::string& url);

  /**
   * Add AWS Signature V4 authentication headers to curl request
   *
   * @param curl_handle Curl handle
   * @param headers Pointer to header list (will be populated)
   * @param url Full request URL
   * @param byte_start Start byte offset
   * @param byte_length Number of bytes
   */
  void add_auth_headers(CURL* curl_handle,
                        struct curl_slist** headers,
                        const std::string& url,
                        uint64_t byte_start,
                        uint64_t byte_length);

  /**
   * Setup curl handle for HTTP GET request with optional range
   *
   * @param url Target URL
   * @param data Output buffer for response data
   * @param byte_start Start byte offset for range request
   * @param byte_length Number of bytes to fetch
   * @param out_headers Output: header list to be freed by caller
   * @return Configured CURL handle
   */
  CURL* setup_curl_handle(const std::string& url, bufferlist* data,
                          uint64_t byte_start, uint64_t byte_length,
                          struct curl_slist** out_headers);

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
