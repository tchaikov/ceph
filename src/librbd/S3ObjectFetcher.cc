// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/S3ObjectFetcher.h"
#include "common/dout.h"
#include "common/errno.h"
#include "include/ceph_assert.h"

#include <curl/curl.h>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::S3ObjectFetcher: " << __func__ << ": "

namespace librbd {

S3ObjectFetcher::S3ObjectFetcher(CephContext* cct)
  : m_cct(cct) {
  ldout(m_cct, 20) << "S3ObjectFetcher created" << dendl;
}

S3ObjectFetcher::~S3ObjectFetcher() {
  ldout(m_cct, 20) << "S3ObjectFetcher destroyed" << dendl;
}

size_t S3ObjectFetcher::write_callback(void* ptr, size_t size, size_t nmemb,
                                        void* userdata) {
  bufferlist* data = static_cast<bufferlist*>(userdata);
  size_t bytes = size * nmemb;

  // Append received data to bufferlist
  data->append(static_cast<char*>(ptr), bytes);

  return bytes;
}

CURL* S3ObjectFetcher::setup_curl_handle(const std::string& url,
                                          bufferlist* data,
                                          uint64_t byte_start,
                                          uint64_t byte_length) {
  CURL* curl_handle = curl_easy_init();
  if (!curl_handle) {
    lderr(m_cct) << "curl_easy_init() failed" << dendl;
    return nullptr;
  }

  // Set URL
  curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());

  // Set HTTP GET method
  curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);

  // Set HTTP Range header if byte range is specified
  if (byte_length > 0) {
    uint64_t byte_end = byte_start + byte_length - 1;
    std::string range_header = std::to_string(byte_start) + "-" + std::to_string(byte_end);
    curl_easy_setopt(curl_handle, CURLOPT_RANGE, range_header.c_str());
    ldout(m_cct, 15) << "setting HTTP Range: bytes=" << range_header << dendl;
  }

  // Set write callback to receive response data
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, data);

  // Timeout settings from configuration
  long timeout_ms = m_cct->_conf.get_val<uint64_t>("rbd_s3_fetch_timeout_ms");
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

  return curl_handle;
}

int S3ObjectFetcher::fetch_with_retry(const std::string& url,
                                       bufferlist* data,
                                       uint32_t max_retries,
                                       uint64_t byte_start,
                                       uint64_t byte_length) {
  int retry_count = 0;
  int last_error = 0;

  while (retry_count <= max_retries) {
    // Clear data buffer for retry
    if (retry_count > 0) {
      data->clear();
      ldout(m_cct, 10) << "retry " << retry_count << "/" << max_retries
                       << " for url: " << url << dendl;
    }

    CURL* curl_handle = setup_curl_handle(url, data, byte_start, byte_length);
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
        curl_easy_cleanup(curl_handle);
        return 0;
      } else if (http_code == 404) {
        lderr(m_cct) << "S3 object not found (404): " << url << dendl;
        curl_easy_cleanup(curl_handle);
        return -ENOENT;
      } else if (http_code == 403) {
        lderr(m_cct) << "S3 access forbidden (403): " << url << dendl;
        curl_easy_cleanup(curl_handle);
        return -EACCES;
      } else if (http_code == 416) {
        lderr(m_cct) << "S3 range not satisfiable (416): " << url << dendl;
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

void S3ObjectFetcher::fetch(const std::string& url,
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

  // Get max retries from configuration
  uint32_t max_retries = cct->_conf.get_val<uint64_t>("rbd_s3_fetch_max_retries");

  // Perform fetch with retry
  int r = fetch_with_retry(url, data, max_retries, byte_start, byte_length);

  // Complete callback
  on_finish->complete(r);
}

} // namespace librbd
