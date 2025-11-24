// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/AWSV4Signer.h"
#include "common/ceph_crypto.h"
#include "common/dout.h"
#include <boost/algorithm/string.hpp>
#include <sstream>
#include <iomanip>

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

std::string AWSV4Signer::get_iso8601_timestamp(time_t t) {
  if (t == 0) {
    t = time(nullptr);
  }

  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);

  char buf[32];
  strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm_buf);
  return std::string(buf);
}

std::string AWSV4Signer::get_date_string(time_t t) {
  if (t == 0) {
    t = time(nullptr);
  }

  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);

  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d", &tm_buf);
  return std::string(buf);
}

std::string AWSV4Signer::sha256_hex(const std::string& data) {
  unsigned char hash[CEPH_CRYPTO_SHA256_DIGESTSIZE];

  ceph::crypto::SHA256 sha256;
  sha256.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  sha256.Final(hash);

  return to_hex(hash, CEPH_CRYPTO_SHA256_DIGESTSIZE);
}

std::vector<unsigned char> AWSV4Signer::hmac_sha256(
    const std::vector<unsigned char>& key,
    const std::string& data) {
  unsigned char digest[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE];

  ceph::crypto::HMACSHA256 hmac(key.data(), key.size());
  hmac.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  hmac.Final(digest);

  return std::vector<unsigned char>(digest, digest + CEPH_CRYPTO_HMACSHA256_DIGESTSIZE);
}

std::vector<unsigned char> AWSV4Signer::hmac_sha256(
    const std::string& key,
    const std::string& data) {
  unsigned char digest[CEPH_CRYPTO_HMACSHA256_DIGESTSIZE];

  ceph::crypto::HMACSHA256 hmac(
    reinterpret_cast<const unsigned char*>(key.data()),
    key.size());
  hmac.Update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  hmac.Final(digest);

  return std::vector<unsigned char>(digest, digest + CEPH_CRYPTO_HMACSHA256_DIGESTSIZE);
}

std::string AWSV4Signer::uri_encode(const std::string& str, bool encode_slash) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (unsigned char c : str) {
    // Keep alphanumeric and other accepted characters intact
    // Use locale-independent character checks instead of isalnum()
    if ((c >= '0' && c <= '9') ||  // digit
        (c >= 'A' && c <= 'Z') ||  // uppercase letter
        (c >= 'a' && c <= 'z') ||  // lowercase letter
        c == '-' || c == '_' || c == '.' || c == '~' ||
        (!encode_slash && c == '/')) {
      escaped << c;
    } else {
      // Percent-encode all other characters
      escaped << std::uppercase;
      escaped << '%' << std::setw(2) << static_cast<int>(c);
      escaped << std::nouppercase;
    }
  }

  return escaped.str();
}

std::string AWSV4Signer::to_hex(const std::vector<unsigned char>& data) {
  return to_hex(data.data(), data.size());
}

std::string AWSV4Signer::to_hex(const unsigned char* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    oss << std::setw(2) << static_cast<unsigned int>(data[i]);
  }
  return oss.str();
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

  // CanonicalHeaders (lowercase and sorted)
  for (const auto& header : headers) {
    canonical_request << boost::algorithm::to_lower_copy(header.first) << ":"
                     << boost::algorithm::trim_copy(header.second) << "\n";
  }
  canonical_request << "\n";

  // SignedHeaders
  canonical_request << signed_headers << "\n";

  // HashedPayload
  canonical_request << payload_hash;

  return canonical_request.str();
}

std::string AWSV4Signer::create_string_to_sign(
    const std::string& timestamp,
    const std::string& scope,
    const std::string& canonical_request_hash) {
  std::ostringstream string_to_sign;

  string_to_sign << "AWS4-HMAC-SHA256\n";
  string_to_sign << timestamp << "\n";
  string_to_sign << scope << "\n";
  string_to_sign << canonical_request_hash;

  return string_to_sign.str();
}

std::vector<unsigned char> AWSV4Signer::calculate_signing_key(
    const std::string& date_string) {
  // kSecret = AWS4 + SecretAccessKey
  std::string k_secret = "AWS4" + m_credentials.secret_key;

  // kDate = HMAC("AWS4" + kSecret, Date)
  auto k_date = hmac_sha256(k_secret, date_string);

  // kRegion = HMAC(kDate, Region)
  auto k_region = hmac_sha256(k_date, m_credentials.region);

  // kService = HMAC(kRegion, Service)
  auto k_service = hmac_sha256(k_region, m_credentials.service);

  // kSigning = HMAC(kService, "aws4_request")
  auto k_signing = hmac_sha256(k_service, "aws4_request");

  return k_signing;
}

std::string AWSV4Signer::calculate_signature(
    const std::vector<unsigned char>& signing_key,
    const std::string& string_to_sign) {
  auto signature = hmac_sha256(signing_key, string_to_sign);
  return to_hex(signature);
}

std::string AWSV4Signer::build_authorization_header(
    const std::string& signed_headers,
    const std::string& scope,
    const std::string& signature) {
  std::ostringstream auth_header;

  auth_header << "AWS4-HMAC-SHA256 ";
  auth_header << "Credential=" << m_credentials.access_key << "/" << scope << ", ";
  auth_header << "SignedHeaders=" << signed_headers << ", ";
  auth_header << "Signature=" << signature;

  return auth_header.str();
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

  // Add additional headers
  for (const auto& header : additional_headers) {
    headers[boost::algorithm::to_lower_copy(header.first)] = header.second;
  }

  // Create signed headers list
  std::ostringstream signed_headers_stream;
  bool first = true;
  for (const auto& header : headers) {
    if (!first) {
      signed_headers_stream << ";";
    }
    signed_headers_stream << boost::algorithm::to_lower_copy(header.first);
    first = false;
  }
  std::string signed_headers = signed_headers_stream.str();

  // Create canonical request
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

  // Populate result
  result.headers["Host"] = host;
  result.headers["x-amz-date"] = iso8601_timestamp;
  result.headers["x-amz-content-sha256"] = payload_hash;

  // Add any additional headers
  for (const auto& header : additional_headers) {
    result.headers[header.first] = header.second;
  }

  result.authorization = authorization;

  return result;
}

} // namespace io
} // namespace librbd
