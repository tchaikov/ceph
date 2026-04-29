// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TOOLS_RBD_BACKFILL_OBJECT_BACKFILL_REQUEST_H
#define CEPH_TOOLS_RBD_BACKFILL_OBJECT_BACKFILL_REQUEST_H

#include "include/Context.h"
#include "include/rados/librados.hpp"
#include "include/int_types.h"
#include "common/Mutex.h"
#include <string>

class CephContext;

namespace rbd {
namespace backfill {

/**
 * ObjectBackfillRequest - State machine for backfilling a single object
 *
 * State flow:
 *   INIT -> ACQUIRE_LOCK -> WRITE_RADOS ->
 *   UPDATE_OBJECT_MAP -> RELEASE_LOCK -> COMPLETE
 *
 * Note: S3 fetch is now done BEFORE creating ObjectBackfillRequest,
 * in the ImageBackfiller thread. This request only handles:
 * - Acquiring distributed lock
 * - Writing data to RADOS
 * - Updating object map
 * - Releasing lock
 */
class ObjectBackfillRequest {
public:
  ObjectBackfillRequest(
    librados::IoCtx& parent_ioctx,
    const std::string& parent_oid,
    uint64_t object_no,
    const ceph::bufferlist& data,  // Pre-fetched data from S3
    const std::string& image_id,   // Parent image ID for object map updates
    CephContext* cct,
    Context* on_finish);

  ~ObjectBackfillRequest();

  // Start the backfill operation
  void send();

private:
  // State machine transitions
  void acquire_lock();
  void handle_acquire_lock(int r);

  void write_rados();
  void handle_write_rados(int r);

  void update_object_map();
  void handle_update_object_map(int r);

  void release_lock();
  void handle_release_lock(int r);

  void finish(int r);

  librados::IoCtx m_parent_ioctx;  // Copy, not reference - must remain valid for async operations
  std::string m_parent_oid;
  std::string m_lock_oid;  // Sentinel object for cls lock (parent_oid + ".s3lk")
                           // Must match CopyupRequest::m_parent_lock_oid
  uint64_t m_object_no;
  std::string m_image_id;          // Parent image ID for object map updates
  CephContext* m_cct;
  Context* m_on_finish;

  mutable Mutex m_lock;
  // True once finish() has run; second/third invocations of finish() (e.g.
  // both the write_rados handler and an unwind path racing it) are ignored.
  bool m_finished = false;
  int m_ret_val = 0;
  bool m_lock_acquired = false;  // Track if we hold the distributed lock

  // Lock management
  std::string m_lock_name;
  std::string m_lock_cookie;
  std::string m_lock_tag;

  // Data buffer for RADOS write (pre-fetched from S3)
  ceph::bufferlist m_data_bl;

  // Lock timeout configuration
  static constexpr uint32_t LOCK_TIMEOUT_SECONDS = 30;
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_TOOLS_RBD_BACKFILL_OBJECT_BACKFILL_REQUEST_H
