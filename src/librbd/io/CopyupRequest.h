// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_COPYUP_REQUEST_H
#define CEPH_LIBRBD_IO_COPYUP_REQUEST_H

#include "include/int_types.h"
#include "include/rados/librados.hpp"
#include "include/buffer.h"
#include "common/Mutex.h"
#include "common/zipkin_trace.h"
#include "librbd/io/AsyncOperation.h"
#include "librbd/io/Types.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace ZTracer { struct Trace; }

namespace librbd {

struct ImageCtx;

namespace io {

class S3ObjectFetcher;
template <typename I> class AbstractObjectWriteRequest;

template <typename ImageCtxT = librbd::ImageCtx>
class CopyupRequest {
public:
  static CopyupRequest* create(ImageCtxT *ictx, const std::string &oid,
                               uint64_t objectno, Extents &&image_extents,
                               const ZTracer::Trace &parent_trace) {
    return new CopyupRequest(ictx, oid, objectno, std::move(image_extents),
                             parent_trace);
  }

  CopyupRequest(ImageCtxT *ictx, const std::string &oid, uint64_t objectno,
                Extents &&image_extents, const ZTracer::Trace &parent_trace);
  ~CopyupRequest();

  void append_request(AbstractObjectWriteRequest<ImageCtxT> *req);

  void send();

private:
  /**
   * Copyup requests go through the following state machine to read from the
   * parent image, update the object map, and copyup the object:
   *
   *
   * @verbatim
   *
   *              <start>
   *                 |
   *      /---------/ \---------\
   *      |                     |
   *      v                     v
   *  READ_FROM_PARENT      DEEP_COPY
   *      |                     |
   *      \---------\ /---------/
   *                 |
   *                 v (skip if not needed)
   *         UPDATE_OBJECT_MAPS
   *                 |
   *                 v (skip if not needed)
   *              COPYUP
   *                 |
   *                 v
   *              <finish>
   *
   * @endverbatim
   *
   * The OBJECT_MAP state is skipped if the object map isn't enabled or if
   * an object map update isn't required. The COPYUP state is skipped if
   * no data was read from the parent *and* there are no additional ops.
   */

  typedef std::vector<AbstractObjectWriteRequest<ImageCtxT> *> WriteRequests;

  ImageCtxT *m_image_ctx;
  std::string m_oid;
  uint64_t m_object_no;
  Extents m_image_extents;
  ZTracer::Trace m_trace;

  bool m_flatten = false;
  bool m_copyup_required = true;
  bool m_copyup_is_zero = true;

  ceph::bufferlist m_copyup_data;

  AsyncOperation m_async_op;

  std::vector<uint64_t> m_snap_ids;
  bool m_first_snap_is_clean = false;

  Mutex m_lock;
  WriteRequests m_pending_requests;
  unsigned m_pending_copyups = 0;

  WriteRequests m_restart_requests;
  bool m_append_request_permitted = true;

  // Result tracking for multi-op copyup: captures the first error seen across
  // all pending RADOS ops so handle_copyup() dispatches exactly once.
  int m_result = 0;

  // Cancellation guard for retry timer / work-queue lambdas.  Set to false
  // just before 'delete this' so stale lambdas (that already escaped their
  // timer or work-queue slot before cancellation) do not access freed memory.
  std::shared_ptr<std::atomic<bool>> m_alive{
    std::make_shared<std::atomic<bool>>(true)};

  // S3 back-fill members
  // Cancellation flag for the inflight S3 curl fetch.  Set to true when the
  // image is closing or the CopyupRequest is being destroyed while a fetch is
  // in-flight, preventing the detached pthread from writing into freed memory.
  std::atomic<bool> m_s3_cancel{false};
  bool m_s3_lock_acquired = false;
  // Set to true only when m_copyup_data was populated from a live S3 fetch
  // (handle_s3_fetch).  When false (data came from do_read_from_parent which
  // reads only m_image_extents), update_parent_object_map_after_copyup must
  // NOT write back to the parent — writing m_copyup_data (partial extents)
  // via write_full would truncate the already-correct 4MB parent RADOS object
  // to just the size of the write extents (e.g. 4KB).
  bool m_data_is_from_s3 = false;
  uint32_t m_s3_retry_count = 0;
  ceph::bufferlist m_s3_data;
  ceph::bufferlist m_lock_info_bl;  // buffer for async get_lock_info response
  std::string m_parent_oid;
  std::string m_parent_lock_oid;  // separate sentinel object for cls lock
  librados::IoCtx m_parent_ioctx;
  // Heap-allocated so its lifetime is tied to CopyupRequest, not the stack
  // frame of fetch_from_s3_async(). The detached pthread writes into m_s3_data
  // which is a member here, so both must outlive the in-flight fetch.
  std::shared_ptr<S3ObjectFetcher> m_s3_fetcher;

  void read_from_parent();
  void handle_read_from_parent(int r);

  void deep_copy();
  void handle_deep_copy(int r);

  void update_object_maps();
  void handle_update_object_maps(int r);

  void copyup();
  void handle_copyup(int r);

  void finish(int r);
  void complete_requests(bool override_restart_retval, int r);

  void disable_append_requests();
  void remove_from_list();

  bool is_copyup_required();
  bool is_update_object_map_required(int r);
  bool is_deep_copy() const;

  void compute_deep_copy_snap_ids();

  // S3 back-fill methods
  bool should_fetch_from_s3();
  void check_parent_object_exists(std::string parent_oid,
                                   librados::IoCtx parent_ioctx);
  void handle_check_parent_object_exists(int r);
  void do_read_from_parent();
  void fetch_from_s3_with_lock();
  void handle_lock_parent_object(int r);
  void try_preempt_backfill_lock();
  void handle_list_lock_holders(int r);
  void handle_break_backfill_lock(int r);
  void retry_read_from_parent();
  void fetch_from_s3_async();
  void handle_s3_fetch(int r);
  void unlock_parent_object();
  void update_parent_object_map_after_copyup();
  void handle_write_parent_after_copyup(int r);
  void handle_update_parent_object_map_after_copyup(int r);
};

} // namespace io
} // namespace librbd

extern template class librbd::io::CopyupRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IO_COPYUP_REQUEST_H
