// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/S3ObjectFetcher.h"
#include "librbd/io/AWSV4Signer.h"
#include "common/dout.h"
#include "common/errno.h"
#include "include/ceph_assert.h"

#include <curl/curl.h>
#include <pthread.h>
#include <regex>
#include <mutex>

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
}

S3ObjectFetcher::S3ObjectFetcher(CephContext* cct, const S3Config& s3_config)
  : m_cct(cct), m_s3_config(s3_config) {
  // Thread-safe initialization of libcurl (called once per process)
  std::call_once(curl_init_flag, init_curl_once);
  ldout(m_cct, 20) << "S3ObjectFetcher created" << dendl;
}

S3ObjectFetcher::~S3ObjectFetcher() {
  ldout(m_cct, 20) << "S3ObjectFetcher destroyed" << dendl;
  // Note: curl_global_cleanup() should only be called once at process exit
}

size_t S3ObjectFetcher::write_callback(void* ptr, size_t size, size_t nmemb,
                                        void* userdata) {
  bufferlist* data = static_cast<bufferlist*>(userdata);
  size_t bytes = size * nmemb;

  // Append received data to bufferlist
  data->append(static_cast<char*>(ptr), bytes);

  return bytes;
}

std::string S3ObjectFetcher::extract_host_from_url(const std::string& url) {
  // Extract host from URL like "http://host:port/path" or "https://host/path"
  std::regex url_regex("^https?://([^/:]+)(:[0-9]+)?(/.*)?$");
  std::smatch match;
  if (std::regex_match(url, match, url_regex)) {
    std::string host = match[1].str();
    if (match[2].matched) {
      // Include port if present
      host += match[2].str();
    }
    return host;
  }
  return "";
}

std::string S3ObjectFetcher::extract_uri_from_url(const std::string& url) {
  // Extract URI path from URL
  std::regex url_regex("^https?://[^/]+(/.*)$");
  std::smatch match;
  if (std::regex_match(url, match, url_regex)) {
    return match[1].str();
  }
  return "/";
}

void S3ObjectFetcher::add_auth_headers(CURL* curl_handle,
                                        struct curl_slist** headers,
                                        const std::string& url,
                                        uint64_t byte_start,
                                        uint64_t byte_length) {
  // Skip authentication if no credentials provided
  if (m_s3_config.is_anonymous()) {
    // NOTE: Logging disabled - this function is called from Finisher thread
    // where thread-local dout_context is not set up
    // ldout(m_cct, 15) << "using anonymous access (no credentials)" << dendl;
    // Just add the Range header manually for anonymous access
    if (byte_length > 0) {
      uint64_t byte_end = byte_start + byte_length - 1;
      std::string range_header = "Range: bytes=" + std::to_string(byte_start) +
                                  "-" + std::to_string(byte_end);
      *headers = curl_slist_append(*headers, range_header.c_str());
    }
    return;
  }

  std::string region = m_s3_config.region;
  if (region.empty()) {
    region = "us-east-1";  // Default region for S3-compatible services
  }

  // NOTE: Logging disabled - this function is called from Finisher thread
  // ldout(m_cct, 10) << "using region for signing: " << region << dendl;
  // ldout(m_cct, 10) << "using access_key: " << m_s3_config.access_key << dendl;
  // ldout(m_cct, 10) << "using secret_key length: " << m_s3_config.secret_key.length() << dendl;

  AWSV4Signer::Credentials creds(
    m_s3_config.access_key,
    m_s3_config.secret_key,
    region,
    "s3"
  );

  AWSV4Signer signer(creds);

  std::string host = extract_host_from_url(url);
  std::string uri = extract_uri_from_url(url);

  // NOTE: Logging disabled - this function is called from Finisher thread
  // ldout(m_cct, 15) << "signing request: host=" << host << ", uri=" << uri << dendl;

  // Build additional headers (Range if needed)
  std::map<std::string, std::string> additional_headers;
  if (byte_length > 0) {
    uint64_t byte_end = byte_start + byte_length - 1;
    std::string range_value = "bytes=" + std::to_string(byte_start) +
                               "-" + std::to_string(byte_end);
    additional_headers["range"] = range_value;
  }

  auto signed_request = signer.sign_request(
    "GET",
    host,
    uri,
    "",  // No query string
    additional_headers,
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"  // SHA256 of empty payload
  );

  // Add all signed headers
  for (const auto& header : signed_request.headers) {
    std::string header_line = header.first + ": " + header.second;
    *headers = curl_slist_append(*headers, header_line.c_str());
    // NOTE: Logging disabled - this function is called from Finisher thread
    // ldout(m_cct, 20) << "adding header: " << header.first << dendl;
  }

  // Add Authorization header
  std::string auth_header = "Authorization: " + signed_request.authorization;
  *headers = curl_slist_append(*headers, auth_header.c_str());
  // NOTE: Logging disabled - this function is called from Finisher thread
  // ldout(m_cct, 15) << "added AWS Signature V4 authorization" << dendl;
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

  *out_headers = nullptr;

  // Set URL
  curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());

  // Set HTTP GET method
  curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);

  // Add AWS Signature V4 authentication headers
  add_auth_headers(curl_handle, out_headers, url, byte_start, byte_length);
  if (*out_headers) {
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, *out_headers);
  }

  // Set write callback to receive response data
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, data);

  // Timeout settings from configuration
  long timeout_ms = m_s3_config.timeout_ms;
  curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, timeout_ms);

  // Low speed timeout (abort if transfer is too slow)
  curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 30L);  // 30 seconds
  curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1024L); // 1KB/s minimum

  // Follow redirects (S3 may redirect)
  curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 3L);

  // SSL verification (enabled by default, can be disabled for testing)
  if (!m_cct->_conf.get_val<bool>("rbd_s3_verify_ssl")) {
    ldout(m_cct, 10) << "SSL verification disabled" << dendl;
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  // Disable signal handlers (required for multi-threaded applications)
  curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);

  // Enable progress meter (for debugging)
  curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);

  // Set user agent
  curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ceph-rbd-s3-fetcher/1.0");

  // Optional: Throttle download speed for testing (0 = unlimited)
  int64_t max_speed = m_cct->_conf.get_val<int64_t>("rbd_s3_max_download_bps");
  if (max_speed > 0) {
    curl_easy_setopt(curl_handle, CURLOPT_MAX_RECV_SPEED_LARGE,
                     (curl_off_t)max_speed);
    ldout(m_cct, 15) << "throttling download to " << max_speed << " bytes/sec" << dendl;
  }

  return curl_handle;
}

int S3ObjectFetcher::fetch_with_retry(const std::string& url,
                                       bufferlist* data,
                                       uint64_t byte_start,
                                       uint64_t byte_length) {
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

    struct curl_slist* headers = nullptr;
    CURL* curl_handle = setup_curl_handle(url, data, byte_start, byte_length, &headers);
    if (!curl_handle) {
      return -ENOMEM;
    }

    // Perform HTTP GET request
    CURLcode res = curl_easy_perform(curl_handle);

    // Get HTTP status code
    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    // Get effective URL (after redirects)
    char* effective_url = nullptr;
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &effective_url);

    if (res == CURLE_OK) {
      // Check HTTP status code - accept both 200 (full content) and 206 (partial content)
      if (http_code == 200 || http_code == 206) {
        ldout(m_cct, 10) << "successfully fetched " << data->length()
                         << " bytes from " << url
                         << " (HTTP " << http_code << ")" << dendl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl_handle);
        return 0;
      } else if (http_code == 404) {
        lderr(m_cct) << "S3 object not found (404): " << url << dendl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl_handle);
        return -ENOENT;
      } else if (http_code == 403) {
        // Log the S3 error response body for debugging
        std::string error_response(data->c_str(), data->length());
        lderr(m_cct) << "S3 access forbidden (403): " << url << dendl;
        lderr(m_cct) << "S3 error response: " << error_response << dendl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl_handle);
        return -EACCES;
      } else if (http_code == 416) {
        lderr(m_cct) << "S3 range not satisfiable (416): " << url << dendl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl_handle);
        return -EINVAL;
      } else if (http_code >= 500 && http_code < 600) {
        // Server error, retry
        ldout(m_cct, 10) << "S3 server error " << http_code
                         << ", will retry" << dendl;
        last_error = -EIO;
      } else {
        lderr(m_cct) << "unexpected HTTP status " << http_code
                     << " from " << url << dendl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl_handle);
        return -EIO;
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
    curl_easy_cleanup(curl_handle);

    // Check if we should retry
    if (retry_count < max_retries) {
      // Exponential backoff: 1s, 2s, 4s
      uint64_t delay_ms = 1000 * (1 << retry_count);
      ldout(m_cct, 10) << "waiting " << delay_ms << "ms before retry" << dendl;

      struct timespec ts;
      ts.tv_sec = delay_ms / 1000;
      ts.tv_nsec = (delay_ms % 1000) * 1000000;
      nanosleep(&ts, nullptr);
    }

    retry_count++;
  }

  lderr(m_cct) << "S3 fetch failed after " << retry_count
               << " attempts: " << url << dendl;
  return last_error;
}

uint64_t S3ObjectFetcher::calculate_s3_offset(uint64_t object_no, uint64_t object_off) {
  // For "raw" format: disk image is stored as a single object
  // Offset = object_number * object_size + object_offset
  ceph_assert(m_s3_config.object_size > 0);
  return (object_no * m_s3_config.object_size) + object_off;
}

std::string S3ObjectFetcher::build_s3_url() {
  // Build URL from S3 config: endpoint/bucket/image_name
  std::string url = m_s3_config.endpoint;
  if (url.back() != '/') {
    url += "/";
  }
  url += m_s3_config.bucket;
  if (!m_s3_config.prefix.empty()) {
    if (url.back() != '/') {
      url += "/";
    }
    url += m_s3_config.prefix;
  }
  if (url.back() != '/') {
    url += "/";
  }
  url += m_s3_config.image_name;
  return url;
}

void* S3ObjectFetcher::async_fetch_thread(void* arg) {
  FetchContext* ctx = static_cast<FetchContext*>(arg);

  // Create multi handle
  CURLM* multi_handle = curl_multi_init();
  if (!multi_handle) {
    ctx->on_finish->complete(-ENOMEM);
    curl_slist_free_all(ctx->headers);
    curl_easy_cleanup(ctx->curl_handle);
    delete ctx;
    return nullptr;
  }

  // Add easy handle to multi handle
  curl_multi_add_handle(multi_handle, ctx->curl_handle);

  // Perform the request using multi interface
  int still_running = 0;
  CURLMcode mc;

  do {
    // Check for cancellation before each iteration
    if (ctx->cancel_flag && ctx->cancel_flag->load()) {
      // Cancellation requested - abort the transfer
      curl_multi_remove_handle(multi_handle, ctx->curl_handle);
      curl_multi_cleanup(multi_handle);
      curl_slist_free_all(ctx->headers);
      curl_easy_cleanup(ctx->curl_handle);
      ctx->on_finish->complete(-ECANCELED);
      delete ctx;
      return nullptr;
    }

    mc = curl_multi_perform(multi_handle, &still_running);

    if (still_running) {
      // Wait for activity, with timeout
      mc = curl_multi_poll(multi_handle, nullptr, 0, 1000, nullptr);
    }

    if (mc != CURLM_OK) {
      break;
    }
  } while (still_running);

  // Check for errors
  int result = 0;
  long response_code = 0;
  curl_easy_getinfo(ctx->curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

  if (mc != CURLM_OK) {
    // libcurl multi interface error
    result = -EIO;
  } else {
    // Check curl easy handle for transfer-level errors
    CURLcode easy_result;
    CURLMsg* msg;
    int msgs_in_queue;
    while ((msg = curl_multi_info_read(multi_handle, &msgs_in_queue))) {
      if (msg->msg == CURLMSG_DONE) {
        easy_result = msg->data.result;
        if (easy_result == CURLE_OPERATION_TIMEDOUT) {
          result = -ETIMEDOUT;
        } else if (easy_result == CURLE_COULDNT_CONNECT) {
          result = -ECONNREFUSED;
        } else if (easy_result != CURLE_OK) {
          result = -EIO;
        }
        break;
      }
    }

    // Check HTTP response code if no curl error
    if (result == 0) {
      if (response_code == 206 || response_code == 200) {
        result = 0;  // Success
      } else if (response_code == 404) {
        result = -ENOENT;
      } else if (response_code >= 500) {
        result = -EIO;  // Server error
      } else if (response_code >= 400) {
        result = -EPERM;  // Client error (auth, forbidden, etc)
      } else if (response_code == 0) {
        result = -EIO;  // No response received
      } else {
        result = -EIO;  // Other HTTP error
      }
    }
  }

  // Clean up
  curl_multi_remove_handle(multi_handle, ctx->curl_handle);
  curl_multi_cleanup(multi_handle);
  curl_slist_free_all(ctx->headers);
  curl_easy_cleanup(ctx->curl_handle);

  // Call completion callback
  ctx->on_finish->complete(result);

  delete ctx;
  return nullptr;
}

void S3ObjectFetcher::fetch_url(const std::string& url,
                                 bufferlist* data,
                                 Context* on_finish,
                                 uint64_t byte_start,
                                 uint64_t byte_length) {
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
  ctx->fetcher = this;
  ctx->url = url;
  ctx->byte_start = byte_start;
  ctx->byte_length = byte_length;
  ctx->out_bl = data;
  ctx->on_finish = on_finish;
  ctx->cancel_flag = nullptr;  // No cancellation for fetch_url

  // Setup curl handle
  ctx->curl_handle = setup_curl_handle(url, data, byte_start, byte_length,
                                       &ctx->headers);
  if (!ctx->curl_handle) {
    ldout(cct, 1) << "failed to setup curl handle" << dendl;
    on_finish->complete(-ENOMEM);
    delete ctx;
    return;
  }

  // Launch async fetch thread
  pthread_t thread_id;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  int r = pthread_create(&thread_id, &attr, async_fetch_thread, ctx);
  pthread_attr_destroy(&attr);

  if (r != 0) {
    ldout(cct, 1) << "failed to create async fetch thread: " << cpp_strerror(r) << dendl;
    curl_slist_free_all(ctx->headers);
    curl_easy_cleanup(ctx->curl_handle);
    on_finish->complete(-r);
    delete ctx;
    return;
  }

  // Thread will handle completion callback and cleanup
  ldout(cct, 15) << "launched async S3 fetch thread" << dendl;
}

void S3ObjectFetcher::fetch(uint64_t object_no, uint64_t object_off,
                            uint64_t length, bufferlist* out_bl,
                            Context* on_finish, std::atomic<bool>* cancel_flag) {
  // Calculate byte offset in S3 object
  uint64_t s3_offset = calculate_s3_offset(object_no, object_off);

  // Build S3 URL
  std::string url = build_s3_url();

  ldout(m_cct, 10) << "fetching object_no=" << object_no
                   << " object_off=" << object_off
                   << " s3_offset=" << s3_offset
                   << " length=" << length << dendl;

  // NOTE: Async fetch with pthread_create disabled for now
  // Using synchronous fetch instead - cancellation not yet supported
  // TODO: Implement proper async fetch with cancellation using work queue
  fetch_url(url, out_bl, on_finish, s3_offset, length);
}

int S3ObjectFetcher::fetch_sync(uint64_t object_no, uint64_t object_off,
                                 uint64_t length, bufferlist* out_bl) {
  // Calculate byte offset in S3 object
  uint64_t s3_offset = calculate_s3_offset(object_no, object_off);

  // Build S3 URL
  std::string url = build_s3_url();

  // Perform HTTP Range GET
  return fetch_with_retry(url, out_bl, s3_offset, length);
}

} // namespace io
} // namespace librbd
