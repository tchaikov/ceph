// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_BACKFILL_TYPES_H
#define CEPH_RBD_BACKFILL_TYPES_H

#include <string>

namespace rbd {
namespace backfill {

// Shared metadata key names used by the backfill daemon, CLI, and librbd.
// Defined here so all components reference the same string without risk of
// diverging silently.
static constexpr const char* BACKFILL_META_NS       = "backfill_";
static constexpr const char* BACKFILL_SCHEDULED_KEY = "backfill_scheduled";
static constexpr const char* BACKFILL_STATUS_KEY    = "backfill_status";

// Values for BACKFILL_SCHEDULED_KEY
static constexpr const char* BACKFILL_SCHED_TRUE        = "true";
static constexpr const char* BACKFILL_SCHED_IN_PROGRESS = "in_progress";

// Values for BACKFILL_STATUS_KEY
static constexpr const char* BACKFILL_STATUS_COMPLETE = "complete";
static constexpr const char* BACKFILL_STATUS_FAILED   = "failed";

struct ImageSpec {
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;
  int64_t pool_id = -1;
  std::string image_id;

  ImageSpec() = default;
  ImageSpec(const std::string& pool, const std::string& image)
    : pool_name(pool), image_name(image) {}

  bool operator<(const ImageSpec& other) const {
    if (pool_id != other.pool_id) {
      return pool_id < other.pool_id;
    }
    if (namespace_name != other.namespace_name) {
      return namespace_name < other.namespace_name;
    }
    return image_id < other.image_id;
  }
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_RBD_BACKFILL_TYPES_H
