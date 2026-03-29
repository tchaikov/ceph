// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/AWSV4Signer.h"
#include "common/ceph_crypto.h"
#include "common/dout.h"
#include "include/stringify.h"
#include <boost/algorithm/string.hpp>
#include <sstream>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::AWSV4Signer: " << __func__ << ": "

namespace librbd {
namespace io {

const std::string AWSV4Signer::UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

AWSV4Signer::AWSV4Signer(const Credentials& credentials)
  : m_credentials(credentials) {
  // Default region if not specified
  if (m_credentials.region.empty()) {
    m_credentials.region = "us-east-1";
  }
}

static std::string format_time(time_t t, const char* fmt, size_t buf_size) {
  if (t == 0) {
    t = time(nullptr);
  }
  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);
  std::string result(buf_size, '\0');
  size_t len = strftime(&result[0], buf_size, fmt, &tm_buf);
  result.resize(len);
  return result;
}

std::string AWSV4Signer::get_iso8601_timestamp(time_t t) {
  return format_time(t, "%Y%m%dT%H%M%SZ", 20);
}

std::string AWSV4Signer::get_date_string(time_t t) {
  return format_time(t, "%Y%m%d", 10);
}

std::string AWSV4Signer::sha256_hex(const std::string& data) {
  unsigned char hash[CEPH_CRYPTO_SHA256_DIGESTSIZE];

  ceph::crypto::SHA256 sha256;
  sha256.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  sha256.Final(hash);

  return to_hex(hash, CEPH_CRYPTO_SHA256_DIGESTSIZE);
}

std::array<unsigned char, 32> AWSV4Signer::hmac_sha256(
    const std::array<unsigned char, 32>& key,
    const std::string& data) {
  std::array<unsigned char, 32> digest;

  ceph::crypto::HMACSHA256 hmac(key.data(), key.size());
  hmac.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  hmac.Final(digest.data());

  return digest;
}

std::array<unsigned char, 32> AWSV4Signer::hmac_sha256(
    const std::string& key,
    const std::string& data) {
  std::array<unsigned char, 32> digest;

  ceph::crypto::HMACSHA256 hmac(
    reinterpret_cast<const unsigned char*>(key.data()),
    key.size());
  hmac.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  hmac.Final(digest.data());

  return digest;
}

std::string AWSV4Signer::uri_encode(const std::string& str, bool encode_slash) {
  static const char upper_hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(str.size() * 3);  // worst case: every byte percent-encoded

  for (unsigned char c : str) {
    if ((c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        c == '-' || c == '_' || c == '.' || c == '~' ||
        (!encode_slash && c == '/')) {
      result += static_cast<char>(c);
    } else {
      result += '%';
      result += upper_hex[c >> 4];
      result += upper_hex[c & 0x0f];
    }
  }

  return result;
}

std::string AWSV4Signer::to_hex(const unsigned char* data, size_t len) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string result;
  result.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    result.push_back(hex_chars[data[i] >> 4]);
    result.push_back(hex_chars[data[i] & 0x0f]);
  }
  return result;
}

std::string AWSV4Signer::create_canonical_request(
    const std::string& method,
    const std::string& uri,
    const std::string& query_string,
    const std::map<std::string, std::string>& headers,
    const std::string& signed_headers,
    const std::string& payload_hash) {
  std::ostringstream canonical_request;

  // HTTPRequestMethod
  canonical_request << method << "\n";

  // CanonicalURI
  canonical_request << uri_encode(uri, false) << "\n";

  // CanonicalQueryString (already sorted and encoded)
  canonical_request << query_string << "\n";

  // CanonicalHeaders — keys are pre-lowercased by the caller; trim values.
  for (const auto& header : headers) {
    canonical_request << header.first << ":"
                     << boost::algorithm::trim_copy(header.second) << "\n";
  }
  canonical_request << "\n";

  // SignedHeaders — passed in from sign_request() to avoid rebuilding.
  canonical_request << signed_headers << "\n";

  // HashedPayload
  canonical_request << payload_hash;

  return canonical_request.str();
}

std::string AWSV4Signer::create_string_to_sign(
    const std::string& timestamp,
    const std::string& scope,
    const std::string& canonical_request_hash) {
  std::string result = "AWS4-HMAC-SHA256\n";
  result += timestamp;
  result += '\n';
  result += scope;
  result += '\n';
  result += canonical_request_hash;
  return result;
}

std::array<unsigned char, 32> AWSV4Signer::calculate_signing_key(
    const std::string& date_string) {
  // The signing key is stable for a full calendar day (changes only when the
  // date rolls over).  Cache it per-thread so repeated requests within the
  // same day skip the four HMAC-SHA256 derivation steps.
  thread_local std::string tl_cached_date;
  thread_local std::array<unsigned char, 32> tl_cached_key;

  if (date_string == tl_cached_date) {
    return tl_cached_key;
  }

  // kSecret = "AWS4" + SecretAccessKey
  std::string k_secret = "AWS4" + m_credentials.secret_key;

  // Derive: kDate → kRegion → kService → kSigning
  auto k_date    = hmac_sha256(k_secret,  date_string);
  auto k_region  = hmac_sha256(k_date,    m_credentials.region);
  auto k_service = hmac_sha256(k_region,  m_credentials.service);
  auto k_signing = hmac_sha256(k_service, "aws4_request");

  tl_cached_date = date_string;
  tl_cached_key  = k_signing;
  return k_signing;
}

std::string AWSV4Signer::calculate_signature(
    const std::array<unsigned char, 32>& signing_key,
    const std::string& string_to_sign) {
  auto signature = hmac_sha256(signing_key, string_to_sign);
  return to_hex(signature.data(), signature.size());
}

std::string AWSV4Signer::build_authorization_header(
    const std::string& signed_headers,
    const std::string& scope,
    const std::string& signature) {
  std::string result = "AWS4-HMAC-SHA256 Credential=";
  result += m_credentials.access_key;
  result += '/';
  result += scope;
  result += ", SignedHeaders=";
  result += signed_headers;
  result += ", Signature=";
  result += signature;
  return result;
}

AWSV4Signer::SignedRequest AWSV4Signer::sign_request(
    const std::string& method,
    const std::string& host,
    const std::string& uri,
    const std::string& query_string,
    const std::map<std::string, std::string>& additional_headers,
    const std::string& payload_hash,
    time_t timestamp) {
  if (timestamp == 0) {
    timestamp = time(nullptr);
  }

  SignedRequest result;

  // Step 1: Create canonical request
  std::string iso8601_timestamp = get_iso8601_timestamp(timestamp);
  std::string date_string = get_date_string(timestamp);

  // Build headers map (lowercase keys, sorted)
  std::map<std::string, std::string> headers;
  headers["host"] = host;
  headers["x-amz-content-sha256"] = payload_hash;
  headers["x-amz-date"] = iso8601_timestamp;

  // Add additional headers (lowercase for signing; original case for result)
  for (const auto& header : additional_headers) {
    headers[boost::algorithm::to_lower_copy(header.first)] = header.second;
    result.headers[header.first] = header.second;
  }

  // Build signed_headers string in one pass — keys are already lowercase
  // and sorted by the std::map iteration order.
  std::string signed_headers;
  for (const auto& header : headers) {
    if (!signed_headers.empty()) {
      signed_headers += ';';
    }
    signed_headers += header.first;
  }

  // Create canonical request — pass signed_headers to avoid rebuilding it.
  std::string canonical_request = create_canonical_request(
    method, uri, query_string, headers, signed_headers, payload_hash);

  // Step 2: Create string to sign
  std::string scope = date_string + "/" + m_credentials.region + "/" +
                     m_credentials.service + "/aws4_request";
  std::string canonical_request_hash = sha256_hex(canonical_request);
  std::string string_to_sign = create_string_to_sign(
    iso8601_timestamp, scope, canonical_request_hash);

  // Step 3: Calculate signature
  auto signing_key = calculate_signing_key(date_string);
  std::string signature = calculate_signature(signing_key, string_to_sign);

  // Step 4: Build Authorization header
  std::string authorization = build_authorization_header(
    signed_headers, scope, signature);

  // Populate base result headers (additional_headers already populated above)
  result.headers["Host"] = host;
  result.headers["x-amz-date"] = iso8601_timestamp;
  result.headers["x-amz-content-sha256"] = payload_hash;
  result.authorization = authorization;

  return result;
}

} // namespace io
} // namespace librbd
