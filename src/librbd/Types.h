// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef LIBRBD_TYPES_H
#define LIBRBD_TYPES_H

#include "include/types.h"
#include "cls/rbd/cls_rbd_types.h"
#include "deep_copy/Types.h"
#include <map>
#include <string>

namespace librbd {

// Performance counters
enum {
  l_librbd_first = 26000,

  l_librbd_rd,               // read ops
  l_librbd_rd_bytes,         // bytes read
  l_librbd_rd_latency,       // average latency
  l_librbd_wr,
  l_librbd_wr_bytes,
  l_librbd_wr_latency,
  l_librbd_discard,
  l_librbd_discard_bytes,
  l_librbd_discard_latency,
  l_librbd_flush,
  l_librbd_flush_latency,

  l_librbd_ws,
  l_librbd_ws_bytes,
  l_librbd_ws_latency,

  l_librbd_cmp,
  l_librbd_cmp_bytes,
  l_librbd_cmp_latency,

  l_librbd_snap_create,
  l_librbd_snap_remove,
  l_librbd_snap_rollback,
  l_librbd_snap_rename,

  l_librbd_notify,
  l_librbd_resize,

  l_librbd_readahead,
  l_librbd_readahead_bytes,

  l_librbd_invalidate_cache,

  // S3-backed parent performance counters
  l_librbd_s3_fetch_count,       // Number of S3 fetch operations
  l_librbd_s3_fetch_bytes,       // Total bytes fetched from S3
  l_librbd_s3_fetch_errors,      // Number of S3 fetch errors

  l_librbd_opened_time,
  l_librbd_lock_acquired_time,

  l_librbd_last,
};

typedef std::map<uint64_t, uint64_t> SnapSeqs;

enum ParentImageType {
  PARENT_TYPE_SNAPSHOT = 0,           // Traditional snapshot-based parent (immutable)
  PARENT_TYPE_STANDALONE = 1,         // Local standalone image parent (same cluster)
  PARENT_TYPE_REMOTE_STANDALONE = 2   // Remote standalone image parent (different cluster)
};

/// Remote cluster connection information for cross-cluster parents
struct RemoteParentSpec {
  std::string cluster_name;
  std::vector<std::string> mon_hosts;
  std::string keyring;  // Base64-encoded

  RemoteParentSpec() = default;
  RemoteParentSpec(const std::string& name, const std::vector<std::string>& mons,
                   const std::string& key)
    : cluster_name(name), mon_hosts(mons), keyring(key) {}

  bool empty() const {
    return cluster_name.empty() && mon_hosts.empty() && keyring.empty();
  }
};

/// Metadata key constants for S3-backed parent image configuration.
/// These are the full rbd_metadata keys as stored via `rbd image-meta set`.
/// S3_META_NS is the common "s3." namespace prefix shared by all S3 keys;
/// use it for metadata_list() filters and starts_with() checks.
static constexpr const char* S3_META_NS             = "s3.";
static constexpr const char* S3_META_KEY_ENABLED    = "s3.enabled";
static constexpr const char* S3_META_KEY_BUCKET     = "s3.bucket";
static constexpr const char* S3_META_KEY_ENDPOINT   = "s3.endpoint";
static constexpr const char* S3_META_KEY_REGION     = "s3.region";
static constexpr const char* S3_META_KEY_PREFIX     = "s3.prefix";
static constexpr const char* S3_META_KEY_ACCESS_KEY = "s3.access_key";
static constexpr const char* S3_META_KEY_SECRET_KEY = "s3.secret_key";
static constexpr const char* S3_META_KEY_IMAGE_NAME = "s3.image_name";
static constexpr const char* S3_META_KEY_IMAGE_FMT  = "s3.image_format";
static constexpr const char* S3_META_KEY_TIMEOUT_MS = "s3.timeout_ms";
static constexpr const char* S3_META_KEY_MAX_RETRIES= "s3.max_retries";

/// Values for S3 metadata keys
static constexpr const char* S3_META_ENABLED_VALUE     = "true";
static constexpr const char* S3_IMAGE_FORMAT_RAW       = "raw";
static constexpr const char* S3_IMAGE_FORMAT_QCOW2     = "qcow2";

/// S3 configuration for S3-backed parent images
// Distributed-lock constants for the S3 fetch path.
// Used by both CopyupRequest (I/O path) and ObjectBackfillRequest (backfill
// daemon); defined here so both sides use the same values without diverging.
static constexpr const char* S3_FETCH_LOCK_NAME          = "s3_fetch_lock";
static constexpr const char* S3_FETCH_LOCK_TAG           = "s3_fetch";
static constexpr const char* S3_FETCH_LOCK_SENTINEL_SUFFIX = ".s3lk";
// Prefix written by ObjectBackfillRequest into its lock cookie; CopyupRequest
// identifies daemon locks by matching this prefix before breaking them.
static constexpr const char* BACKFILL_LOCK_COOKIE_PREFIX = "backfill-";

struct S3Config {
  bool enabled = false;
  std::string bucket;
  std::string endpoint;
  std::string region;
  std::string access_key;
  std::string secret_key;  // Note: stored as base64-encoded in metadata, decoded at runtime
  uint32_t timeout_ms = 30000;
  uint32_t max_retries = 3;
  std::string prefix;
  std::string image_name;   // Name of the image object in S3 bucket
  std::string image_format; // Format of the image (currently only "raw" supported)
  S3Config() = default;

  /// Check if S3 configuration is valid (all required fields present)
  bool is_valid() const {
    return enabled &&
           !bucket.empty() &&
           !endpoint.empty() &&
           !image_name.empty() &&
           !image_format.empty();
    // region and prefix are also optional
  }

  /// Build full S3 URL for the image object (cached; URL never changes after config load).
  const std::string& build_url() const {
    if (m_cached_url.empty()) {
      std::string url = endpoint;
      if (url.empty() || url.back() != '/') {
        url += '/';
      }
      url += bucket;
      url += '/';
      if (!prefix.empty()) {
        url += prefix;
        if (url.back() != '/') {
          url += '/';
        }
      }
      url += image_name;
      m_cached_url = std::move(url);
    }
    return m_cached_url;
  }

private:
  mutable std::string m_cached_url;
};

/// Full information about an image's parent.
struct ParentImageInfo {
  /// Identification of the parent.
  cls::rbd::ParentImageSpec spec;

  /** @brief Where the portion of data shared with the child image ends.
   * Since images can be resized multiple times, the portion of data shared
   * with the child image is not necessarily min(parent size, child size).
   * If the child image is first shrunk and then enlarged, the common portion
   * will be shorter. */
  uint64_t overlap = 0;

  /// Type of parent (snapshot, local standalone, or remote standalone)
  ParentImageType parent_type = PARENT_TYPE_SNAPSHOT;

  /// Remote cluster information (for REMOTE_STANDALONE parent type)
  RemoteParentSpec remote;
};

struct SnapInfo {
  std::string name;
  cls::rbd::SnapshotNamespace snap_namespace;
  uint64_t size;
  ParentImageInfo parent;
  uint8_t protection_status;
  uint64_t flags;
  utime_t timestamp;
  SnapInfo(std::string _name,
           const cls::rbd::SnapshotNamespace &_snap_namespace,
           uint64_t _size, const ParentImageInfo &_parent,
           uint8_t _protection_status, uint64_t _flags, utime_t _timestamp)
    : name(_name), snap_namespace(_snap_namespace), size(_size),
      parent(_parent), protection_status(_protection_status), flags(_flags),
      timestamp(_timestamp) {
  }
};

enum {
  OPEN_FLAG_SKIP_OPEN_PARENT = 1 << 0,
  OPEN_FLAG_OLD_FORMAT       = 1 << 1,
  OPEN_FLAG_IGNORE_MIGRATING = 1 << 2
};

struct MigrationInfo {
  int64_t pool_id = -1;
  std::string pool_namespace;
  std::string image_name;
  std::string image_id;
  deep_copy::SnapMap snap_map;
  uint64_t overlap = 0;
  bool flatten = false;

  MigrationInfo() {
  }
  MigrationInfo(int64_t pool_id, const std::string& pool_namespace,
                const std::string& image_name, const std::string& image_id,
                const deep_copy::SnapMap &snap_map, uint64_t overlap,
                bool flatten)
    : pool_id(pool_id), pool_namespace(pool_namespace), image_name(image_name),
      image_id(image_id), snap_map(snap_map), overlap(overlap),
      flatten(flatten) {
  }

  bool empty() const {
    return pool_id == -1;
  }
};

} // namespace librbd

#endif // LIBRBD_TYPES_H
