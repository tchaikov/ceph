// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_AWSV4_SIGNER_H
#define CEPH_LIBRBD_IO_AWSV4_SIGNER_H

#include <string>
#include <map>
#include <vector>
#include <ctime>

namespace librbd {
namespace io {

/**
 * AWS Signature Version 4 signer for S3 requests.
 *
 * Implements the AWS Signature Version 4 signing process for authenticating
 * requests to S3-compatible object storage services.
 *
 * Reference: https://docs.aws.amazon.com/general/latest/gr/signature-version-4.html
 */
class AWSV4Signer {
public:
  struct Credentials {
    std::string access_key;
    std::string secret_key;
    std::string region;       // e.g., "us-east-1"
    std::string service;      // e.g., "s3"

    Credentials() : service("s3") {}
    Credentials(const std::string& ak, const std::string& sk,
                const std::string& reg = "us-east-1",
                const std::string& svc = "s3")
      : access_key(ak), secret_key(sk), region(reg), service(svc) {}

    bool is_valid() const {
      return !access_key.empty() && !secret_key.empty();
    }
  };

  struct SignedRequest {
    std::map<std::string, std::string> headers;
    std::string authorization;
  };

  AWSV4Signer(const Credentials& credentials);
  ~AWSV4Signer() = default;

  /**
   * Sign an HTTP request using AWS Signature V4.
   *
   * @param method HTTP method (GET, PUT, etc.)
   * @param host Host header value (bucket.endpoint or endpoint)
   * @param uri Request URI path (e.g., "/bucket/key")
   * @param query_string Query string without leading '?' (optional)
   * @param additional_headers Additional headers to include in signature
   * @param payload_hash SHA256 hash of request payload (use UNSIGNED_PAYLOAD for GET)
   * @param timestamp Optional timestamp (uses current time if not provided)
   * @return SignedRequest containing headers to add to the request
   */
  SignedRequest sign_request(
    const std::string& method,
    const std::string& host,
    const std::string& uri,
    const std::string& query_string,
    const std::map<std::string, std::string>& additional_headers,
    const std::string& payload_hash,
    time_t timestamp = 0);

  /**
   * Get the current timestamp in ISO8601 format for x-amz-date header.
   * Format: YYYYMMDD'T'HHMMSS'Z'
   */
  static std::string get_iso8601_timestamp(time_t t = 0);

  /**
   * Get date string in YYYYMMDD format.
   */
  static std::string get_date_string(time_t t = 0);

  /**
   * Constant for unsigned payload (used for GET requests).
   */
  static const std::string UNSIGNED_PAYLOAD;

  /**
   * Calculate SHA256 hash of data and return hex string.
   */
  static std::string sha256_hex(const std::string& data);

  /**
   * Calculate HMAC-SHA256 and return raw bytes.
   */
  static std::vector<unsigned char> hmac_sha256(
    const std::vector<unsigned char>& key,
    const std::string& data);

  /**
   * Calculate HMAC-SHA256 with string key and return raw bytes.
   */
  static std::vector<unsigned char> hmac_sha256(
    const std::string& key,
    const std::string& data);

private:
  Credentials m_credentials;

  /**
   * Create canonical request string.
   */
  std::string create_canonical_request(
    const std::string& method,
    const std::string& uri,
    const std::string& query_string,
    const std::map<std::string, std::string>& headers,
    const std::string& signed_headers,
    const std::string& payload_hash);

  /**
   * Create string to sign.
   */
  std::string create_string_to_sign(
    const std::string& timestamp,
    const std::string& scope,
    const std::string& canonical_request_hash);

  /**
   * Calculate signing key.
   * SigningKey = HMAC-SHA256(HMAC-SHA256(HMAC-SHA256(HMAC-SHA256("AWS4" + SecretKey, Date), Region), Service), "aws4_request")
   */
  std::vector<unsigned char> calculate_signing_key(const std::string& date_string);

  /**
   * Calculate signature.
   */
  std::string calculate_signature(
    const std::vector<unsigned char>& signing_key,
    const std::string& string_to_sign);

  /**
   * Build Authorization header value.
   */
  std::string build_authorization_header(
    const std::string& signed_headers,
    const std::string& scope,
    const std::string& signature);

  /**
   * URI encode a string (AWS-style).
   */
  static std::string uri_encode(const std::string& str, bool encode_slash = true);

  /**
   * Convert bytes to hex string.
   */
  static std::string to_hex(const std::vector<unsigned char>& data);
  static std::string to_hex(const unsigned char* data, size_t len);
};

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_AWSV4_SIGNER_H
