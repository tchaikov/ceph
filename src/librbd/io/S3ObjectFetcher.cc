// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/S3ObjectFetcher.h"
#include "common/dout.h"
#include "common/errno.h"
#include <curl/curl.h>
#include <pthread.h>
#include <sstream>
#include <iomanip>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::S3ObjectFetcher: " << __func__ << ": "

namespace librbd {
namespace io {

// Callback for libcurl to write received data
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t total_size = size * nmemb;
  ceph::bufferlist* bl = static_cast<ceph::bufferlist*>(userp);
  bl->append(static_cast<char*>(contents), total_size);
  return total_size;
}

S3ObjectFetcher::S3ObjectFetcher(const S3Config& config)
  : m_config(config) {
  // Initialize libcurl globally (should be done once per process)
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

S3ObjectFetcher::~S3ObjectFetcher() {
  // Note: curl_global_cleanup() should only be called once at process exit
}

uint64_t S3ObjectFetcher::calculate_s3_offset(uint64_t object_no, uint64_t object_off) {
  // For "raw" format: disk image is stored as a single object
  // Offset = object_number * object_size + object_offset
  return (object_no * m_config.object_size) + object_off;
}

int S3ObjectFetcher::do_http_range_get(uint64_t offset, uint64_t length,
                                        ceph::bufferlist* out_bl) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return -ENOMEM;
  }

  // Construct S3 URL
  std::stringstream url;
  url << m_config.endpoint << "/" << m_config.bucket_name << "/" << m_config.object_key;

  // Construct Range header: "bytes=start-end"
  std::stringstream range_header;
  range_header << "Range: bytes=" << offset << "-" << (offset + length - 1);

  // Set up curl options
  curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_bl);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // 30 second timeout

  // Add headers
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, range_header.str().c_str());

  // For MinIO with no auth (development mode), we can skip AWS v4 signature
  // In production, would need to implement AWS v4 signature generation
  if (!m_config.access_key.empty() && !m_config.secret_key.empty()) {
    // For now, just add basic auth headers
    // TODO: Implement proper AWS v4 signature
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_config.access_key.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_config.secret_key.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Perform the request
  CURLcode res = curl_easy_perform(curl);

  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  // Clean up
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    return -EIO;
  }

  // Check HTTP response code
  if (response_code == 206) {
    // 206 Partial Content - success
    return 0;
  } else if (response_code == 200) {
    // 200 OK - server doesn't support range requests, but returned full object
    // This is acceptable if the object is small
    return 0;
  } else if (response_code == 404) {
    return -ENOENT;
  } else {
    return -EIO;
  }
}

int S3ObjectFetcher::fetch_sync(uint64_t object_no, uint64_t object_off,
                                 uint64_t length, ceph::bufferlist* out_bl) {
  // Calculate byte offset in S3 object
  uint64_t s3_offset = calculate_s3_offset(object_no, object_off);

  // Perform HTTP Range GET
  return do_http_range_get(s3_offset, length, out_bl);
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

void S3ObjectFetcher::fetch(uint64_t object_no, uint64_t object_off,
                            uint64_t length, ceph::bufferlist* out_bl,
                            Context* on_finish) {
  // Calculate byte offset in S3 object
  uint64_t s3_offset = calculate_s3_offset(object_no, object_off);

  // Set up curl handle
  CURL* curl = curl_easy_init();
  if (!curl) {
    on_finish->complete(-ENOMEM);
    return;
  }

  // Construct S3 URL
  std::stringstream url;
  url << m_config.endpoint << "/" << m_config.bucket_name << "/" << m_config.object_key;

  // Construct Range header
  std::stringstream range_header_str;
  range_header_str << "Range: bytes=" << s3_offset << "-" << (s3_offset + length - 1);

  // Set up curl options
  curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_bl);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  // Timeout settings
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);          // Total timeout: 60s
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);   // Connection timeout: 10s
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // Min speed: 1KB/s
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);    // Low speed timeout: 30s

  // Add headers
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, range_header_str.str().c_str());

  // Authentication if configured
  if (!m_config.access_key.empty() && !m_config.secret_key.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, m_config.access_key.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, m_config.secret_key.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Create context for async operation
  FetchContext* ctx = new FetchContext{
    this,
    s3_offset,
    length,
    out_bl,
    on_finish,
    curl,
    headers
  };

  // Launch thread to handle async operation
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  int r = pthread_create(&thread, &attr, async_fetch_thread, ctx);
  pthread_attr_destroy(&attr);

  if (r != 0) {
    // Thread creation failed
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    delete ctx;
    on_finish->complete(-r);
  }
}

} // namespace io
} // namespace librbd
