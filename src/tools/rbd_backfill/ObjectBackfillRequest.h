// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TOOLS_RBD_BACKFILL_OBJECT_BACKFILL_REQUEST_H
#define CEPH_TOOLS_RBD_BACKFILL_OBJECT_BACKFILL_REQUEST_H

#include "include/int_types.h"
#include "include/Context.h"
#include "include/rados/librados.hpp"
#include "librbd/io/S3ObjectFetcher.h"
#include "common/Mutex.h"
#include <atomic>
#include <string>

class CephContext;

namespace rbd {
namespace backfill {

class Threads;

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
    CephContext* cct,
    Threads* threads,
    Context* on_finish);

  ~ObjectBackfillRequest();

  // Start the backfill operation
  void send();

  // Cancel the operation (called when user IO needs the lock)
  void cancel();

private:
  enum State {
    STATE_INIT,
    STATE_ACQUIRE_LOCK,
    STATE_WRITE_RADOS,
    STATE_UPDATE_OBJECT_MAP,
    STATE_RELEASE_LOCK,
    STATE_COMPLETE,
    STATE_ERROR
  };

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

  // Context wrappers for async operations
  class C_Request : public Context {
  public:
    C_Request(ObjectBackfillRequest* request, void (ObjectBackfillRequest::*method)(int))
      : m_request(request), m_method(method) {}
    void finish(int r) override {
      (m_request->*m_method)(r);
    }
  private:
    ObjectBackfillRequest* m_request;
    void (ObjectBackfillRequest::*m_method)(int);
  };

  librados::IoCtx m_parent_ioctx;  // Copy, not reference - must remain valid for async operations
  std::string m_parent_oid;
  uint64_t m_object_no;
  Threads* m_threads;
  CephContext* m_cct;
  Context* m_on_finish;

  mutable Mutex m_lock;
  State m_state;
  int m_ret_val;
  std::atomic<bool> m_cancel_flag;
  bool m_lock_acquired;  // Track if we hold the distributed lock
  bool m_finished;       // Prevent double-finish

  // Lock management
  std::string m_lock_name;
  std::string m_lock_cookie;
  std::string m_lock_tag;
  uint64_t m_watch_handle;

  // Data buffer for RADOS write (pre-fetched from S3)
  ceph::bufferlist m_data_bl;

  // Lock timeout configuration
  static constexpr uint32_t LOCK_TIMEOUT_SECONDS = 30;
  static constexpr const char* LOCK_OWNER = "rbd-backfill";
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_TOOLS_RBD_BACKFILL_OBJECT_BACKFILL_REQUEST_H
