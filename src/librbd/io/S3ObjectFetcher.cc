// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/S3ObjectFetcher.h"
#include "librbd/io/AWSV4Signer.h"
#include "common/dout.h"
#include "common/errno.h"
#include "include/ceph_assert.h"

#include <curl/curl.h>
#include <chrono>
#include <mutex>
#include <thread>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::S3ObjectFetcher: " << __func__ << ": "

namespace librbd {
namespace io {

namespace {
  // Thread-safe initialization of libcurl (must be called exactly once per process)
  std::once_flag curl_init_flag;
  void init_curl_once() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  }

  // Map an HTTP response code to a POSIX errno.
  // Used by both the sync (fetch_with_retry) and async (async_fetch_thread) paths.
  // Returns 0 for success (200/206), negative errno for errors.
  // Returns INT_MIN to signal "should retry" for 5xx server errors.
  static constexpr int RETRY_SIGNAL = INT_MIN;
  int http_code_to_errno(long http_code) {
    if (http_code == 200 || http_code == 206) return 0;
    if (http_code == 404) return -ENOENT;
    if (http_code == 403) return -EACCES;
    if (http_code == 416) return -EINVAL;
    if (http_code >= 500 && http_code < 600) return RETRY_SIGNAL;
    if (http_code >= 400) return -EPERM;
    return -EIO;
  }

  // Concurrency limiter: cap simultaneous S3 HTTP connections to avoid
  // overloading the S3 server when many child clones trigger COW reads
  // simultaneously. Excess threads block here (cheaply) until a slot frees.
  static constexpr int S3_MAX_CONCURRENT_FETCHES = 8;
  std::mutex s3_fetch_mutex;
  std::condition_variable s3_fetch_cv;
  int s3_active_fetches = 0;
}

void S3ObjectFetcher::share_lock(CURL*, curl_lock_data data,
                                 curl_lock_access, void* userptr) {
  auto* fetcher = static_cast<S3ObjectFetcher*>(userptr);
  fetcher->m_share_mutexes[data].lock();
}

void S3ObjectFetcher::share_unlock(CURL*, curl_lock_data data, void* userptr) {
  auto* fetcher = static_cast<S3ObjectFetcher*>(userptr);
  fetcher->m_share_mutexes[data].unlock();
}

S3ObjectFetcher::S3ObjectFetcher(CephContext* cct, const S3Config& s3_config)
  : m_cct(cct),
    m_s3_config(s3_config),
    m_signer(AWSV4Signer::Credentials(s3_config.access_key, s3_config.secret_key,
                                      s3_config.region, "s3")),
    m_verify_ssl(cct->_conf.get_val<bool>("rbd_s3_verify_ssl")),
    m_max_download_bps(cct->_conf.get_val<int64_t>("rbd_s3_max_download_bps")) {
  // Thread-safe initialization of libcurl (called once per process)
  std::call_once(curl_init_flag, init_curl_once);

  // Create a shared connection/DNS pool for all easy handles this fetcher
  // creates.  The share lets libcurl reuse idle keep-alive connections across
  // separate requests, eliminating the TCP+TLS handshake on every object fetch.
  m_curl_share = curl_share_init();
  if (m_curl_share) {
    curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(m_curl_share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(m_curl_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(m_curl_share, CURLSHOPT_USERDATA, this);
  }
  ldout(m_cct, 20) << "S3ObjectFetcher created" << dendl;
}

S3ObjectFetcher::~S3ObjectFetcher() {
  ldout(m_cct, 20) << "S3ObjectFetcher destroyed" << dendl;
  if (m_sync_handle) {
    curl_easy_cleanup(m_sync_handle);
    m_sync_handle = nullptr;
  }
  if (m_curl_share) {
    curl_share_cleanup(m_curl_share);
    m_curl_share = nullptr;
  }
  // Note: curl_global_cleanup() should only be called once at process exit
}

size_t S3ObjectFetcher::write_callback(void* ptr, size_t size, size_t nmemb,
                                        void* userdata) {
  // Guard against integer overflow: libcurl guarantees size==1 in practice,
  // but the API allows arbitrary values, so check before multiplying.
  if (size != 0 && nmemb > SIZE_MAX / size) {
    return 0;  // Signal error to libcurl (causes CURLE_WRITE_ERROR)
  }
  bufferlist* data = static_cast<bufferlist*>(userdata);
  size_t bytes = size * nmemb;
  data->append(static_cast<char*>(ptr), bytes);
  return bytes;
}

std::string S3ObjectFetcher::extract_host_from_url(const std::string& url) {
  // Extract host[:port] from URL like "http://host:port/path" or "https://host/path".
  // Avoid std::regex to prevent data-race risks with static regex objects on
  // older libstdc++ versions used in the Nautilus-era build toolchain.
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return "";
  }
  auto host_start = scheme_end + 3;
  // Host ends at the first '/' or end-of-string after the authority component.
  auto host_end = url.find('/', host_start);
  return url.substr(host_start,
                    host_end == std::string::npos ? std::string::npos
                                                  : host_end - host_start);
}

std::string S3ObjectFetcher::extract_uri_from_url(const std::string& url) {
  // Extract URI path from URL (the part after the authority component).
  // Avoid std::regex for the same reason as extract_host_from_url.
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return "/";
  }
  auto path_start = url.find('/', scheme_end + 3);
  if (path_start == std::string::npos) {
    return "/";
  }
  return url.substr(path_start);
}

void S3ObjectFetcher::add_auth_headers(CURL* curl_handle,
                                        struct curl_slist** headers,
                                        const std::string& url,
                                        uint64_t byte_start,
                                        uint64_t byte_length) {
  // Skip authentication if no credentials provided
  if (m_s3_config.is_anonymous()) {
    ldout(m_cct, 15) << "using anonymous access (no credentials)" << dendl;
    // Just add the Range header manually for anonymous access
    if (byte_length > 0) {
      uint64_t byte_end = byte_start + byte_length - 1;
      std::string range_header = "Range: bytes=" + std::to_string(byte_start) +
                                  "-" + std::to_string(byte_end);
      *headers = curl_slist_append(*headers, range_header.c_str());
    }
    return;
  }

  std::string host = extract_host_from_url(url);
  std::string uri = extract_uri_from_url(url);

  ldout(m_cct, 15) << "signing request: host=" << host << ", uri=" << uri << dendl;

  // Build additional headers (Range if needed)
  std::map<std::string, std::string> additional_headers;
  if (byte_length > 0) {
    uint64_t byte_end = byte_start + byte_length - 1;
    additional_headers["range"] = "bytes=" + std::to_string(byte_start) +
                                  "-" + std::to_string(byte_end);
  }

  auto signed_request = m_signer.sign_request(
    "GET", host, uri,
    "",  // No query string
    additional_headers,
    AWSV4Signer::UNSIGNED_PAYLOAD);

  // Add all signed headers
  for (const auto& header : signed_request.headers) {
    std::string header_line = header.first + ": " + header.second;
    *headers = curl_slist_append(*headers, header_line.c_str());
    ldout(m_cct, 20) << "adding header: " << header.first << dendl;
  }

  // Add Authorization header
  std::string auth_header = "Authorization: " + signed_request.authorization;
  *headers = curl_slist_append(*headers, auth_header.c_str());
  ldout(m_cct, 15) << "added AWS Signature V4 authorization" << dendl;
}

void S3ObjectFetcher::apply_curl_options(CURL* handle,
                                          const std::string& url,
                                          bufferlist* data,
                                          uint64_t byte_start,
                                          uint64_t byte_length,
                                          struct curl_slist** out_headers) {
  *out_headers = nullptr;
  add_auth_headers(handle, out_headers, url, byte_start, byte_length);
  if (*out_headers) {
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, *out_headers);
  }
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, data);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)m_s3_config.timeout_ms);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);   // 30 seconds
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L); // 1 KB/s minimum
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 3L);
  if (!m_verify_ssl) {
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "ceph-rbd-s3-fetcher/1.0");
  if (m_curl_share) {
    curl_easy_setopt(handle, CURLOPT_SHARE, m_curl_share);
  }
  if (m_max_download_bps > 0) {
    curl_easy_setopt(handle, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)m_max_download_bps);
  }
}

CURL* S3ObjectFetcher::setup_curl_handle(const std::string& url,
                                          bufferlist* data,
                                          uint64_t byte_start,
                                          uint64_t byte_length,
                                          struct curl_slist** out_headers) {
  CURL* curl_handle = curl_easy_init();
  if (!curl_handle) {
    lderr(m_cct) << "curl_easy_init() failed" << dendl;
    return nullptr;
  }
  apply_curl_options(curl_handle, url, data, byte_start, byte_length, out_headers);
  return curl_handle;
}

int S3ObjectFetcher::fetch_with_retry(const std::string& url,
                                       bufferlist* data,
                                       uint64_t byte_start,
                                       uint64_t byte_length) {
  // NOTE: This synchronous path (used by the backfill daemon via fetch_sync)
  // does NOT hold a slot in s3_active_fetches.  The backfill daemon serialises
  // S3 fetches within each ImageBackfiller thread (one fetch at a time per
  // image), so BackfillThrottler implicitly caps the per-image concurrency.
  // Cross-image concurrency (N images × 1 fetch) is acceptable load on S3.
  int retry_count = 0;
  int last_error = 0;
  uint32_t max_retries = m_s3_config.max_retries;

  while (retry_count <= max_retries) {
    // Clear data buffer for retry
    if (retry_count > 0) {
      data->clear();
      ldout(m_cct, 10) << "retry " << retry_count << "/" << max_retries
                       << " for url: " << url << dendl;
    }

    // Reuse the persistent sync handle: curl_easy_reset() clears all per-request
    // settings but leaves the TCP connection alive in the share's pool so the
    // next iteration (and the next call to fetch_sync) can reuse it.
    if (!m_sync_handle) {
      m_sync_handle = curl_easy_init();
      if (!m_sync_handle) {
        return -ENOMEM;
      }
    } else {
      curl_easy_reset(m_sync_handle);
    }
    struct curl_slist* headers = nullptr;
    apply_curl_options(m_sync_handle, url, data, byte_start, byte_length, &headers);

    // Perform HTTP GET request
    CURLcode res = curl_easy_perform(m_sync_handle);

    // Get HTTP status code
    long http_code = 0;
    curl_easy_getinfo(m_sync_handle, CURLINFO_RESPONSE_CODE, &http_code);

    // Get effective URL (after redirects)
    char* effective_url = nullptr;
    curl_easy_getinfo(m_sync_handle, CURLINFO_EFFECTIVE_URL, &effective_url);

    if (res == CURLE_OK) {
      int r = http_code_to_errno(http_code);
      if (r == 0) {
        ldout(m_cct, 10) << "successfully fetched " << data->length()
                         << " bytes from " << url
                         << " (HTTP " << http_code << ")" << dendl;
        curl_slist_free_all(headers);
        // Keep m_sync_handle alive for reuse — do NOT call curl_easy_cleanup
        return 0;
      } else if (r == RETRY_SIGNAL) {
        // Server error, retry
        ldout(m_cct, 10) << "S3 server error " << http_code
                         << ", will retry" << dendl;
        last_error = -EIO;
      } else {
        if (http_code == 403) {
          std::string error_response(data->c_str(), data->length());
          lderr(m_cct) << "S3 access forbidden (403): " << url
                       << " response: " << error_response << dendl;
        } else {
          lderr(m_cct) << "S3 HTTP error " << http_code << " from " << url << dendl;
        }
        curl_slist_free_all(headers);
        return r;
      }
    } else {
      // Curl error
      const char* error_msg = curl_easy_strerror(res);

      if (res == CURLE_OPERATION_TIMEDOUT) {
        ldout(m_cct, 10) << "S3 fetch timeout: " << error_msg << dendl;
        last_error = -ETIMEDOUT;
      } else if (res == CURLE_COULDNT_RESOLVE_HOST ||
                 res == CURLE_COULDNT_CONNECT) {
        ldout(m_cct, 10) << "S3 connection failed: " << error_msg << dendl;
        last_error = -EHOSTUNREACH;
      } else {
        lderr(m_cct) << "curl error: " << error_msg << " (" << res << ")"
                     << dendl;
        last_error = -EIO;
      }
    }

    curl_slist_free_all(headers);
    // m_sync_handle is retained for the next retry / next call

    // Check if we should retry
    if (retry_count < max_retries) {
      // Exponential backoff with jitter: base_delay * (1 ± 25%)
      // This prevents thundering herd when many requests fail simultaneously
      uint64_t base_delay_ms = 1000 * (1 << retry_count);  // 1s, 2s, 4s

      // Add deterministic ±25% jitter based on retry_count to prevent
      // thundering herd without relying on rand() which is not thread-safe.
      // Pattern: +25%, 0%, -25%, repeating.
      int jitter_range = static_cast<int>(base_delay_ms / 4);
      int jitter = (retry_count % 3 == 0) ? jitter_range :
                   (retry_count % 3 == 1) ? 0 : -jitter_range;
      uint64_t delay_ms = base_delay_ms + jitter;

      ldout(m_cct, 10) << "waiting " << delay_ms << "ms before retry "
                       << "(base=" << base_delay_ms << "ms, jitter=" << jitter << "ms)" << dendl;

      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    retry_count++;
  }

  lderr(m_cct) << "S3 fetch failed after " << retry_count
               << " attempts: " << url << dendl;
  return last_error;
}

uint64_t S3ObjectFetcher::calculate_s3_offset(uint64_t object_no, uint64_t object_off) const {
  // For "raw" format: disk image is stored as a single object
  // Offset = object_number * object_size + object_offset
  ceph_assert(m_s3_config.object_size > 0);
  return (object_no * m_s3_config.object_size) + object_off;
}



void* S3ObjectFetcher::async_fetch_thread(void* arg) {
  FetchContext* ctx = static_cast<FetchContext*>(arg);

  // Acquire a concurrency slot before performing any network I/O.
  // This prevents S3 server overload when many clones trigger simultaneous
  // COW reads from the same parent (e.g., 4 VMs booting concurrently).
  {
    std::unique_lock<std::mutex> lock(s3_fetch_mutex);
    s3_fetch_cv.wait(lock,
      [] { return s3_active_fetches < S3_MAX_CONCURRENT_FETCHES; });
    ++s3_active_fetches;
  }

  // Check for cancellation before issuing any I/O.
  if (ctx->cancel_flag && ctx->cancel_flag->load()) {
    {
      std::unique_lock<std::mutex> lock(s3_fetch_mutex);
      --s3_active_fetches;
    }
    s3_fetch_cv.notify_one();
    curl_slist_free_all(ctx->headers);
    curl_easy_cleanup(ctx->curl_handle);
    ctx->on_finish->complete(-ECANCELED);
    delete ctx;
    return nullptr;
  }

  // Perform the blocking HTTP GET.  curl_easy_perform() is the correct
  // single-transfer API; curl_multi_*() adds overhead with no benefit here.
  CURLcode res = curl_easy_perform(ctx->curl_handle);

  long response_code = 0;
  curl_easy_getinfo(ctx->curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

  int result;
  if (res == CURLE_OPERATION_TIMEDOUT) {
    result = -ETIMEDOUT;
  } else if (res == CURLE_COULDNT_CONNECT) {
    result = -ECONNREFUSED;
  } else if (res != CURLE_OK) {
    result = -EIO;
  } else {
    result = http_code_to_errno(response_code);
    // 5xx from the async path: treat as generic I/O error (no retry here;
    // the async path is fire-and-forget — retries are the caller's concern).
    if (result == RETRY_SIGNAL) {
      result = -EIO;
    }
  }

  curl_slist_free_all(ctx->headers);
  curl_easy_cleanup(ctx->curl_handle);

  // Release concurrency slot before invoking the completion callback so a
  // waiting thread can start its S3 fetch while we handle the result.
  {
    std::unique_lock<std::mutex> lock(s3_fetch_mutex);
    --s3_active_fetches;
  }
  s3_fetch_cv.notify_one();

  ctx->on_finish->complete(result);
  delete ctx;
  return nullptr;
}

void S3ObjectFetcher::fetch_url(const std::string& url,
                                 bufferlist* data,
                                 Context* on_finish,
                                 uint64_t byte_start,
                                 uint64_t byte_length,
                                 std::shared_ptr<std::atomic<bool>> cancel_flag) {
  auto cct = m_cct;

  if (byte_length > 0) {
    ldout(cct, 10) << "fetching from S3: " << url
                   << " range: bytes=" << byte_start << "-"
                   << (byte_start + byte_length - 1) << dendl;
  } else {
    ldout(cct, 10) << "fetching from S3: " << url << " (full object)" << dendl;
  }

  ceph_assert(data != nullptr);
  ceph_assert(on_finish != nullptr);

  // Clear output buffer
  data->clear();

  // Create fetch context for async operation
  FetchContext* ctx = new FetchContext();
  ctx->url = url;
  ctx->byte_start = byte_start;
  ctx->byte_length = byte_length;
  ctx->out_bl = data;
  ctx->on_finish = on_finish;
  ctx->cancel_flag = cancel_flag;

  // Setup curl handle
  ctx->curl_handle = setup_curl_handle(url, data, byte_start, byte_length,
                                       &ctx->headers);
  if (!ctx->curl_handle) {
    ldout(cct, 1) << "failed to setup curl handle" << dendl;
    on_finish->complete(-ENOMEM);
    delete ctx;
    return;
  }

  // Launch async fetch thread — detached so it cleans up automatically
  try {
    std::thread([ctx]() { async_fetch_thread(ctx); }).detach();
  } catch (const std::system_error& e) {
    lderr(cct) << "failed to create async fetch thread: " << e.what() << dendl;
    curl_slist_free_all(ctx->headers);
    curl_easy_cleanup(ctx->curl_handle);
    on_finish->complete(-ENOMEM);
    delete ctx;
    return;
  }

  ldout(cct, 15) << "launched async S3 fetch thread" << dendl;
}

void S3ObjectFetcher::fetch(uint64_t object_no, uint64_t object_off,
                            uint64_t length, bufferlist* out_bl,
                            Context* on_finish,
                            std::shared_ptr<std::atomic<bool>> cancel_flag) {
  // Calculate byte offset in S3 object
  uint64_t s3_offset = calculate_s3_offset(object_no, object_off);

  // Build S3 URL
  std::string url = m_s3_config.build_url();

  ldout(m_cct, 10) << "fetching object_no=" << object_no
                   << " object_off=" << object_off
                   << " s3_offset=" << s3_offset
                   << " length=" << length << dendl;

  fetch_url(url, out_bl, on_finish, s3_offset, length, cancel_flag);
}

int S3ObjectFetcher::fetch_sync(uint64_t object_no, uint64_t object_off,
                                 uint64_t length, bufferlist* out_bl) {
  // Calculate byte offset in S3 object
  uint64_t s3_offset = calculate_s3_offset(object_no, object_off);

  // Build S3 URL
  std::string url = m_s3_config.build_url();

  // Perform HTTP Range GET
  return fetch_with_retry(url, out_bl, s3_offset, length);
}

} // namespace io
} // namespace librbd
