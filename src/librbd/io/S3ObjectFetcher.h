// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H
#define CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H

#include "include/buffer.h"
#include "include/Context.h"
#include "librbd/io/AWSV4Signer.h"
#include "librbd/Types.h"
#include <curl/curl.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declaration only for curl_slist; CURL is defined via the header above.
struct curl_slist;

class CephContext;

namespace librbd {
namespace io {

/**
 * S3ObjectFetcher - Unified HTTP client for fetching objects from S3-compatible storage
 *
 * This class provides object fetching from S3 storage using libcurl with AWS Signature V4 auth.
 *
 * Supports two usage patterns:
 * 1. URL-based fetching: fetch_url() - for direct URL fetching (used by ObjectRequest/CopyupRequest)
 * 2. Object-based fetching: fetch() - calculates URL from RBD object number (used by backfill daemon)
 *
 * Key features:
 * - AWS Signature V4 authentication
 * - Anonymous S3 access (when no credentials provided)
 * - Configurable timeout and retry handling
 * - Cancellation support for async operations
 * - libcurl-based (reuses Ceph's existing dependency)
 */
class S3ObjectFetcher {
public:
  /**
   * Construct S3ObjectFetcher with S3 configuration
   *
   * @param cct Ceph context for logging and configuration
   * @param s3_config S3 configuration including credentials and endpoints
   */
  S3ObjectFetcher(CephContext* cct, const S3Config& s3_config);

  ~S3ObjectFetcher();

  /**
   * Fetch data from S3 using explicit URL (for ObjectRequest/CopyupRequest)
   *
   * Performs an HTTP GET request to fetch an object from S3 storage.
   * Supports HTTP range requests for fetching partial objects.
   *
   * @param url Full S3 URL (e.g., "https://bucket.s3.amazonaws.com/key")
   * @param data Output buffer to store fetched data
   * @param on_finish Completion callback (called with return code)
   * @param byte_start Start byte offset for range request (0 = beginning)
   * @param byte_length Number of bytes to fetch (0 = fetch entire object)
   */
  void fetch_url(
    const std::string& url,
    bufferlist* data,
    Context* on_finish,
    uint64_t byte_start = 0,
    uint64_t byte_length = 0,
    std::shared_ptr<std::atomic<bool>> cancel_flag = nullptr);

  /**
   * Fetch data from S3 parent image using RBD object number (for backfill daemon)
   *
   * Calculates S3 byte offset from RBD object number and performs HTTP range GET.
   * Requires s3_config.object_size to be set for offset calculation.
   *
   * @param object_no RBD object number
   * @param object_off Offset within the RBD object
   * @param length Number of bytes to read
   * @param out_bl Output buffer to fill with data
   * @param on_finish Completion callback (called with result code)
   * @param cancel_flag Optional shared cancellation flag (nullptr = no cancellation).
   *        Shared ownership ensures the flag remains valid for the duration of
   *        the async fetch even if the caller is destroyed before it completes.
   */
  void fetch(uint64_t object_no, uint64_t object_off, uint64_t length,
             bufferlist* out_bl, Context* on_finish,
             std::shared_ptr<std::atomic<bool>> cancel_flag = nullptr);

  /**
   * Synchronous fetch using RBD object number (for testing)
   */
  int fetch_sync(uint64_t object_no, uint64_t object_off, uint64_t length,
                 bufferlist* out_bl);

private:
  CephContext* m_cct;
  S3Config m_s3_config;
  AWSV4Signer m_signer;  // Pre-built once from s3_config credentials

  // Shared connection/DNS cache for all easy handles created by this fetcher.
  // HTTP keep-alive: libcurl keeps idle connections open in this pool so
  // subsequent requests to the same S3 endpoint reuse the TCP (and TLS)
  // connection without paying the handshake cost again.
  CURLSH* m_curl_share = nullptr;

  // Per-data-type mutexes required by libcurl for thread-safe share access.
  std::mutex m_share_mutexes[CURL_LOCK_DATA_LAST];

  // Persistent easy handle for the synchronous fetch path (fetch_with_retry).
  // The backfill daemon calls fetch_sync() sequentially from one thread; reusing
  // this handle lets the connection remain open between consecutive object fetches.
  CURL* m_sync_handle = nullptr;

  // Config values cached at construction to avoid per-request config lookups.
  bool m_verify_ssl = true;
  int64_t m_max_download_bps = 0;

  // Host and URI path extracted from the S3 URL once at construction.
  // These never change after the fetcher is created, so caching avoids
  // re-parsing the URL string on every request.
  std::string m_cached_host;
  std::string m_cached_uri;

  struct FetchContext {
    bufferlist* out_bl;
    Context* on_finish;
    CURL* curl_handle;
    struct curl_slist* headers;
    // Shared ownership ensures the flag remains valid for the lifetime of the
    // async fetch even if the caller is destroyed before it completes.
    std::shared_ptr<std::atomic<bool>> cancel_flag;
  };

  // Fixed-size thread pool for async S3 fetches.
  // Threads are created at construction and joined at destruction, so the
  // pool provides clean lifecycle management with no per-request overhead.
  std::vector<std::thread> m_worker_threads;
  std::deque<FetchContext*> m_work_queue;
  std::mutex m_work_mutex;
  std::condition_variable m_work_cv;
  bool m_shutdown = false;

  // libcurl share lock/unlock callbacks (called with 'this' as userptr)
  static void share_lock(CURL*, curl_lock_data data, curl_lock_access, void* userptr);
  static void share_unlock(CURL*, curl_lock_data data, void* userptr);

  // Calculate byte offset in S3 object from RBD object number
  uint64_t calculate_s3_offset(uint64_t object_no, uint64_t object_off) const;

  // Perform HTTP Range GET request with retry logic
  int fetch_with_retry(const std::string& url, bufferlist* data,
                      uint64_t byte_start, uint64_t byte_length);

  // Setup curl handle for HTTP GET request (allocates a new easy handle)
  CURL* setup_curl_handle(const std::string& url, bufferlist* data,
                          uint64_t byte_start, uint64_t byte_length,
                          struct curl_slist** out_headers);

  // Apply all per-request curl options to an existing handle.
  // Shared by setup_curl_handle (new handle) and fetch_with_retry (reused handle).
  // Caller must call curl_slist_free_all(*out_headers) after the request.
  void apply_curl_options(CURL* handle, const std::string& url, bufferlist* data,
                          uint64_t byte_start, uint64_t byte_length,
                          struct curl_slist** out_headers);

  // Add AWS Signature V4 authentication headers to curl request.
  // Uses m_cached_host and m_cached_uri (computed at construction) for signing.
  void add_auth_headers(CURL* curl_handle, struct curl_slist** headers,
                       uint64_t byte_start, uint64_t byte_length);

  // Extract host from URL (including port if present)
  static std::string extract_host_from_url(const std::string& url);

  // Extract URI path from URL
  static std::string extract_uri_from_url(const std::string& url);

  // libcurl write callback for response data
  static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata);

  // Worker thread loop: dequeues FetchContext items and executes them.
  void worker_thread_body();

  // Execute a single fetch: performs the HTTP GET and invokes on_finish.
  void execute_fetch(FetchContext* ctx);
};

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H
