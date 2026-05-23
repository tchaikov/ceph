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
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
   * @param object_size RBD object size in bytes; required only when fetch() is
   *        used (backfill path).  Callers that use only fetch_url() may omit it.
   */
  S3ObjectFetcher(CephContext* cct, const S3Config& s3_config,
                  uint64_t object_size = 0);

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
   * Requires object_size > 0 (passed to constructor) for offset calculation.
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

  // RBD object size used by fetch() to convert object number → S3 byte offset.
  // Only needed by the backfill path; zero for CopyupRequest/ObjectRequest callers.
  uint64_t m_object_size = 0;

  // Config values cached at construction to avoid per-request config lookups.
  bool m_verify_ssl = true;
  int64_t m_max_download_bps = 0;

  // URL, host, and URI path computed once at construction — never change.
  std::string m_cached_url;
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

    // In-flight dedup: when non-empty, this fetch is the "primary" for the
    // given URL+range key, and any concurrent caller asking for the same
    // bytes attaches as a waiter instead of issuing its own GET.  The
    // primary is responsible for copying the fetched data into each
    // waiter's bufferlist and completing their callbacks after the GET
    // returns (or fails).  Empty == no coalescing (fetch is unique).
    std::string coalesce_key;
    std::vector<std::pair<bufferlist*, Context*>> waiters;
  };

  // Fixed-size thread pool for async S3 fetches.
  // Threads are created at construction and joined at destruction, so the
  // pool provides clean lifecycle management with no per-request overhead.
  std::vector<std::thread> m_worker_threads;
  std::deque<FetchContext*> m_work_queue;
  std::mutex m_work_mutex;
  std::condition_variable m_work_cv;
  bool m_shutdown = false;

  // In-flight fetch coalescing: maps coalesce_key (url@start-end) to the
  // primary FetchContext.  Concurrent callers asking for the same URL+range
  // attach to the primary's waiters list instead of issuing a duplicate GET.
  // This handles SIMULTANEOUS overlapping fetches; sequential overlaps are
  // handled by the recent-fetch cache below.
  std::mutex m_inflight_mutex;
  std::unordered_map<std::string, FetchContext*> m_inflight;

  // Recent-fetch in-process cache.  After a fetch completes, its bytes
  // sit here for RECENT_FETCH_TTL_MS so closely-spaced SEQUENTIAL reads
  // of the same URL+range (e.g., rbd bench --io-size 64K --io-total 1M
  // = 16 sequential sub-reads of one 4 MB object) hit memory in 0 RTTs
  // instead of refetching.
  //
  // Why this exists: ObjectReadRequest's design moved the cache-populate
  // write_full off the client critical path (into AsyncWritebackThrottler).
  // The trade-off is that subsequent reads can't rely on RADOS being
  // populated yet — without this cache they'd each fire their own S3 GET.
  // This cache fills that gap purely in-process, without restoring the
  // pre-fix design's wait-for-write-full latency on the client critical
  // path.
  //
  // TTL of 2s is sized to: (a) cover the worst-case 16-sub-read sequence
  // (typically <1 s); (b) be short enough that stale data doesn't pile
  // up across unrelated read bursts; (c) overlap with the AsyncWriteback
  // Throttler's RADOS-populate window (~100-300 ms), so once the cache
  // entry expires the natural RADOS read path is warm.
  //
  // Entry cap of 32 × 4 MB = 128 MB worst case per process; bufferlist's
  // shallow copy means real memory is typically much less (refcounted
  // buffer sharing with curl's response buffers).
  static constexpr size_t RECENT_FETCH_MAX_ENTRIES = 32;
  static constexpr uint32_t RECENT_FETCH_TTL_MS = 2000;
  struct CachedEntry {
    // shared_ptr so multiple concurrent readers can copy out of the same
    // underlying bufferlist without holding the cache mutex.
    std::shared_ptr<ceph::bufferlist> data;
    std::chrono::steady_clock::time_point expires_at;
  };
  // List is insertion-ordered (newest at back); LRU touch moves to back;
  // over-cap eviction pops from front.
  mutable std::mutex m_recent_mutex;
  std::list<std::pair<std::string, CachedEntry>> m_recent_lru;
  std::unordered_map<std::string,
                     std::list<std::pair<std::string, CachedEntry>>::iterator>
      m_recent_index;

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

  // Add AWS Signature V4 authentication headers to a curl request's
  // header list.  Uses m_cached_host and m_cached_uri (computed at
  // construction) for signing.
  void add_auth_headers(struct curl_slist** headers,
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

  // Free curl resources, detach from the in-flight map, and complete the
  // primary callback followed by all coalesced waiters with the given result.
  // On success (result == 0) the fetched bytes are copied into each waiter's
  // bufferlist before its callback fires, AND the bytes are recorded in
  // the recent-fetch cache for sequential-read dedup.  Always deletes ctx.
  void complete_and_destroy(FetchContext* ctx, int result);

  // Returns true and serves the response from m_recent_lru iff the cache
  // has a non-expired entry for coalesce_key.  Side effect on hit: copies
  // bytes into *out_bl, LRU-touches the entry, and fires on_finish inline
  // (caller does not deref `this` after fetch_url returns, so inline
  // completion is safe).  Returns false on miss or expiry (and evicts the
  // expired entry).
  bool try_serve_from_recent(const std::string& coalesce_key,
                             ceph::bufferlist* out_bl,
                             Context* on_finish);

  // Insert a freshly-completed fetch result into the recent cache.
  // Updates LRU position if the key was already present.  Evicts the
  // oldest entry once the map exceeds RECENT_FETCH_MAX_ENTRIES.
  void cache_recent(const std::string& coalesce_key,
                    const ceph::bufferlist& data);
};

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_S3_OBJECT_FETCHER_H
