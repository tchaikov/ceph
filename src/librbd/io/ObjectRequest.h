// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_OBJECT_REQUEST_H
#define CEPH_LIBRBD_IO_OBJECT_REQUEST_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "include/rados/librados.hpp"
#include "common/snap_types.h"
#include "common/zipkin_trace.h"
#include "librbd/ObjectMap.h"
#include "librbd/io/Types.h"
#include <atomic>
#include <map>
#include <memory>

class Context;
class ObjectExtent;

namespace librbd {

struct ImageCtx;

namespace io {

struct AioCompletion;
template <typename> class CopyupRequest;
class S3ObjectFetcher;

/**
 * This class represents an I/O operation to a single RBD data object.
 * Its subclasses encapsulate logic for dealing with special cases
 * for I/O due to layering.
 */
template <typename ImageCtxT = ImageCtx>
class ObjectRequest {
public:
  static ObjectRequest* create_write(
      ImageCtxT *ictx, const std::string &oid, uint64_t object_no,
      uint64_t object_off, ceph::bufferlist&& data, const ::SnapContext &snapc,
      int op_flags, const ZTracer::Trace &parent_trace, Context *completion);
  static ObjectRequest* create_discard(
      ImageCtxT *ictx, const std::string &oid, uint64_t object_no,
      uint64_t object_off, uint64_t object_len, const ::SnapContext &snapc,
      int discard_flags, const ZTracer::Trace &parent_trace,
      Context *completion);
  static ObjectRequest* create_write_same(
      ImageCtxT *ictx, const std::string &oid, uint64_t object_no,
      uint64_t object_off, uint64_t object_len, ceph::bufferlist&& data,
      const ::SnapContext &snapc, int op_flags,
      const ZTracer::Trace &parent_trace, Context *completion);
  static ObjectRequest* create_compare_and_write(
      ImageCtxT *ictx, const std::string &oid, uint64_t object_no,
      uint64_t object_off, ceph::bufferlist&& cmp_data,
      ceph::bufferlist&& write_data, const ::SnapContext &snapc,
      uint64_t *mismatch_offset, int op_flags,
      const ZTracer::Trace &parent_trace, Context *completion);

  ObjectRequest(ImageCtxT *ictx, const std::string &oid,
                uint64_t objectno, uint64_t off, uint64_t len,
                librados::snap_t snap_id, const char *trace_name,
                const ZTracer::Trace &parent_trace, Context *completion);
  virtual ~ObjectRequest() {
    m_trace.event("finish");
  }

  static void add_write_hint(ImageCtxT& image_ctx,
                             librados::ObjectWriteOperation *wr);

  virtual void send() = 0;

  bool has_parent() const {
    return m_has_parent;
  }

  virtual const char *get_op_type() const = 0;

protected:
  bool compute_parent_extents(Extents *parent_extents, bool read_request);

  ImageCtxT *m_ictx;
  std::string m_oid;
  uint64_t m_object_no, m_object_off, m_object_len;
  librados::snap_t m_snap_id;
  Context *m_completion;
  ZTracer::Trace m_trace;

  void async_finish(int r);
  void finish(int r);

private:
  bool m_has_parent = false;
};

template <typename ImageCtxT = ImageCtx>
class ObjectReadRequest : public ObjectRequest<ImageCtxT> {
public:
  typedef std::map<uint64_t, uint64_t> ExtentMap;

  static ObjectReadRequest* create(ImageCtxT *ictx, const std::string &oid,
                                   uint64_t objectno, uint64_t offset,
                                   uint64_t len, librados::snap_t snap_id,
                                   int op_flags,
                                   const ZTracer::Trace &parent_trace,
                                   ceph::bufferlist* read_data,
                                   ExtentMap* extent_map, Context *completion) {
    return new ObjectReadRequest(ictx, oid, objectno, offset, len,
                                 snap_id, op_flags, parent_trace, read_data,
                                 extent_map, completion);
  }

  ObjectReadRequest(ImageCtxT *ictx, const std::string &oid,
                    uint64_t objectno, uint64_t offset, uint64_t len,
                    librados::snap_t snap_id, int op_flags,
                    const ZTracer::Trace &parent_trace,
                    ceph::bufferlist* read_data, ExtentMap* extent_map,
                    Context *completion);

  // Set m_cancelled BEFORE the base ~ObjectRequest runs so any
  // still-pending S3 fetch worker callback (captured m_cancelled by
  // shared_ptr value) bails before dereferencing `this`.
  ~ObjectReadRequest() override;

  void send() override;

  const char *get_op_type() const override {
    return "read";
  }

  // Fire-and-forget readahead helper: spawn an ObjectReadRequest that
  // routes through the normal read pipeline (read_object -> RADOS, fall
  // through to read_from_s3 -> handle_read_from_s3 which submits the
  // throttler, populates the in-process LRU, and updates object_map).
  // Unlike S3ObjectFetcher::prefetch_next, this populates the durable
  // RADOS cache too -- without it, prefetched data lives only in the
  // per-process LRU and is lost when the process exits, forcing the
  // next process to re-fetch from S3 (caught by warm_cache_zero_overhead).
  // The spawned request has m_is_prefetch=true to suppress recursive
  // prefetch chains and self-deletes its throwaway read_data + extent_map
  // when the completion fires.
  static void spawn_prefetch(ImageCtxT *ictx, uint64_t target_object_no);

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    |
   *    v
   * READ_OBJECT
   *    |
   *    v (skip if not needed)
   * READ_PARENT
   *    |
   *    v (skip if not needed)
   * COPYUP
   *    |
   *    v
   * <finish>
   *
   * @endverbatim
   */

  int m_op_flags;

  ceph::bufferlist* m_read_data;
  ExtentMap* m_extent_map;

  // Shared reference to the image's S3 fetcher (lazily initialized in
  // image_ctx->s3_fetcher under snap_lock).  Holding a shared_ptr here keeps
  // the fetcher alive for the duration of the async fetch even if the image
  // is closed concurrently (e.g., during flatten).
  std::shared_ptr<S3ObjectFetcher> m_s3_fetcher;

  // Cancellation flag passed to S3ObjectFetcher::fetch_url so that an
  // in-flight S3 worker can bail before it dereferences this ObjectRead
  // Request (or, transitively, image_ctx fields like perfcounter)
  // after the owning ImageCtx has been closed.  Captured by value
  // (shared_ptr) into the fetcher's worker context; survives this
  // request's destruction.  Mirrors CopyupRequest::m_cancelled.
  std::shared_ptr<std::atomic<bool>> m_cancelled{
    std::make_shared<std::atomic<bool>>(false)};

  // Set by S3ObjectFetcher::fetch_url to true iff the response was
  // served inline from the in-process LRU cache (no actual S3 GET).
  // handle_read_from_s3 reads this and SKIPS the throttler submission
  // and prefetch — the original fetch already submitted them, and
  // re-submitting just burns RADOS cls_lock acquire+EBUSY-drop ops
  // that pollute pool write_ops (caught by warm_cache_zero_overhead).
  bool m_fetched_from_cache = false;

  // Set true by spawn_prefetch so handle_read_from_s3 suppresses its own
  // recursive readahead -- otherwise each level of prefetch would compound
  // into an unbounded fanout (depth K fires K^N reads).
  bool m_is_prefetch = false;

  void read_object();
  void handle_read_object(int r);

  void read_parent();
  void handle_read_parent(int r);

  void copyup();

  // Client I/O critical path on the read side stays minimal:
  //   read RADOS → on miss → S3 GET → return data + submit detached
  //   cache writeback to AsyncWritebackThrottler.
  // The throttler-owned WritebackRequest handles cls_lock acquire,
  // write_full of the parent oid, and obj_map update entirely off
  // the client's critical path.
  bool should_read_from_s3();
  void read_from_s3();
  void handle_read_from_s3(int r);

  // On a transient S3 fetch failure (5xx, network blip — anything not
  // -ENOENT / -EINVAL which indicates a sparse-image legitimate miss),
  // try ONE RADOS read of m_oid before propagating the error.  A peer's
  // throttler-owned writeback may have populated the parent oid during
  // our S3 failure window; if so we serve the peer's data instead of
  // returning the S3 error to a guest VM as EIO.  Stashed S3 error
  // surfaces only if RADOS also misses.  Leaf path: ALWAYS calls
  // this->finish(); never re-enters S3.
  int m_s3_fallback_err = 0;
  void try_rados_fallback_on_s3_error(int s3_err);
  void handle_rados_fallback(int r);
};

template <typename ImageCtxT = ImageCtx>
class AbstractObjectWriteRequest : public ObjectRequest<ImageCtxT> {
public:
  AbstractObjectWriteRequest(ImageCtxT *ictx, const std::string &oid,
                             uint64_t object_no, uint64_t object_off,
                             uint64_t len, const ::SnapContext &snapc,
			     const char *trace_name,
			     const ZTracer::Trace &parent_trace,
                             Context *completion);

  virtual bool is_empty_write_op() const {
    return false;
  }

  virtual uint8_t get_pre_write_object_map_state() const {
    return OBJECT_EXISTS;
  }

  virtual void add_copyup_ops(librados::ObjectWriteOperation *wr) {
    add_write_ops(wr);
  }

  void handle_copyup(int r);

  void send() override;

protected:
  bool m_full_object = false;
  bool m_copyup_enabled = true;

  virtual bool is_no_op_for_nonexistent_object() const {
    return false;
  }
  virtual bool is_object_map_update_enabled() const {
    return true;
  }
  virtual bool is_post_copyup_write_required() const {
    return false;
  }
  virtual bool is_non_existent_post_write_object_map_state() const {
    return false;
  }

  virtual void add_write_hint(librados::ObjectWriteOperation *wr);
  virtual void add_write_ops(librados::ObjectWriteOperation *wr) = 0;

  virtual int filter_write_result(int r) const {
    return r;
  }

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v           (no-op write request)
   * DETECT_NO_OP . . . . . . . . . . . . . . . . . . .
   *    |                                             .
   *    v (skip if not required/disabled)             .
   * PRE_UPDATE_OBJECT_MAP                            .
   *    |          .                                  .
   *    |          . (child dne)                      .
   *    |          . . . . . . . . .                  .
   *    |                          .                  .
   *    |   (post-copyup write)    .                  .
   *    | . . . . . . . . . . . .  .                  .
   *    | .                     .  .                  .
   *    v v                     .  v                  .
   *   WRITE . . . . . . . . > COPYUP (if required)   .
   *    |                       |                     .
   *    |/----------------------/                     .
   *    |                                             .
   *    v (skip if not required/disabled)             .
   * POST_UPDATE_OBJECT_MAP                           .
   *    |                                             .
   *    v                                             .
   * <finish> < . . . . . . . . . . . . . . . . . . . .
   *
   * @endverbatim
   */

  uint64_t m_snap_seq;
  std::vector<librados::snap_t> m_snaps;

  Extents m_parent_extents;
  bool m_object_may_exist = false;
  bool m_copyup_in_progress = false;
  bool m_guarding_migration_write = false;

  void compute_parent_info();

  void pre_write_object_map_update();
  void handle_pre_write_object_map_update(int r);

  void write_object();
  void handle_write_object(int r);

  void copyup();

  void post_write_object_map_update();
  void handle_post_write_object_map_update(int r);

};

template <typename ImageCtxT = ImageCtx>
class ObjectWriteRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectWriteRequest(ImageCtxT *ictx, const std::string &oid,
                     uint64_t object_no, uint64_t object_off,
                     ceph::bufferlist&& data, const ::SnapContext &snapc,
                     int op_flags, const ZTracer::Trace &parent_trace,
                     Context *completion)
    : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                            data.length(), snapc, "write",
                                            parent_trace, completion),
      m_write_data(std::move(data)), m_op_flags(op_flags) {
  }

  bool is_empty_write_op() const override {
    return (m_write_data.length() == 0);
  }

  const char *get_op_type() const override {
    return "write";
  }

protected:
  void add_write_ops(librados::ObjectWriteOperation *wr) override;

private:
  ceph::bufferlist m_write_data;
  int m_op_flags;
};

template <typename ImageCtxT = ImageCtx>
class ObjectDiscardRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectDiscardRequest(ImageCtxT *ictx, const std::string &oid,
                       uint64_t object_no, uint64_t object_off,
                       uint64_t object_len, const ::SnapContext &snapc,
                       int discard_flags, const ZTracer::Trace &parent_trace,
                       Context *completion)
    : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                            object_len, snapc, "discard",
                                            parent_trace, completion),
      m_discard_flags(discard_flags) {
    if (this->m_full_object) {
      if ((m_discard_flags & OBJECT_DISCARD_FLAG_DISABLE_CLONE_REMOVE) != 0 &&
          this->has_parent()) {
        if (!this->m_copyup_enabled) {
          // need to hide the parent object instead of child object
          m_discard_action = DISCARD_ACTION_REMOVE_TRUNCATE;
        } else {
          m_discard_action = DISCARD_ACTION_TRUNCATE;
        }
        this->m_object_len = 0;
      } else {
        m_discard_action = DISCARD_ACTION_REMOVE;
      }
    } else if (object_off + object_len == ictx->layout.object_size) {
      m_discard_action = DISCARD_ACTION_TRUNCATE;
    } else {
      m_discard_action = DISCARD_ACTION_ZERO;
    }
  }

  const char* get_op_type() const override {
    switch (m_discard_action) {
    case DISCARD_ACTION_REMOVE:
      return "remove";
    case DISCARD_ACTION_REMOVE_TRUNCATE:
      return "remove (create+truncate)";
    case DISCARD_ACTION_TRUNCATE:
      return "truncate";
    case DISCARD_ACTION_ZERO:
      return "zero";
    }
    ceph_abort();
    return nullptr;
  }

  uint8_t get_pre_write_object_map_state() const override {
    if (m_discard_action == DISCARD_ACTION_REMOVE) {
      return OBJECT_PENDING;
    }
    return OBJECT_EXISTS;
  }

protected:
  bool is_no_op_for_nonexistent_object() const override {
    return (!this->has_parent());
  }
  bool is_object_map_update_enabled() const override {
    return (
      (m_discard_flags & OBJECT_DISCARD_FLAG_DISABLE_OBJECT_MAP_UPDATE) == 0);
  }
  bool is_non_existent_post_write_object_map_state() const override {
    return (m_discard_action == DISCARD_ACTION_REMOVE);
  }

  void add_write_hint(librados::ObjectWriteOperation *wr) override {
    // no hint for discard
  }

  void add_write_ops(librados::ObjectWriteOperation *wr) override {
    switch (m_discard_action) {
    case DISCARD_ACTION_REMOVE:
      wr->remove();
      break;
    case DISCARD_ACTION_REMOVE_TRUNCATE:
      wr->create(false);
      // fall through
    case DISCARD_ACTION_TRUNCATE:
      wr->truncate(this->m_object_off);
      break;
    case DISCARD_ACTION_ZERO:
      wr->zero(this->m_object_off, this->m_object_len);
      break;
    default:
      ceph_abort();
      break;
    }
  }

private:
  enum DiscardAction {
    DISCARD_ACTION_REMOVE,
    DISCARD_ACTION_REMOVE_TRUNCATE,
    DISCARD_ACTION_TRUNCATE,
    DISCARD_ACTION_ZERO
  };

  DiscardAction m_discard_action;
  int m_discard_flags;

};

template <typename ImageCtxT = ImageCtx>
class ObjectWriteSameRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectWriteSameRequest(ImageCtxT *ictx, const std::string &oid,
			 uint64_t object_no, uint64_t object_off,
			 uint64_t object_len, ceph::bufferlist&& data,
                         const ::SnapContext &snapc, int op_flags,
			 const ZTracer::Trace &parent_trace,
			 Context *completion)
    : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                            object_len, snapc, "writesame",
                                            parent_trace, completion),
      m_write_data(std::move(data)), m_op_flags(op_flags) {
  }

  const char *get_op_type() const override {
    return "writesame";
  }

protected:
  void add_write_ops(librados::ObjectWriteOperation *wr) override;

private:
  ceph::bufferlist m_write_data;
  int m_op_flags;
};

template <typename ImageCtxT = ImageCtx>
class ObjectCompareAndWriteRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectCompareAndWriteRequest(ImageCtxT *ictx, const std::string &oid,
                               uint64_t object_no, uint64_t object_off,
                               ceph::bufferlist&& cmp_bl,
                               ceph::bufferlist&& write_bl,
                               const ::SnapContext &snapc,
                               uint64_t *mismatch_offset, int op_flags,
                               const ZTracer::Trace &parent_trace,
                               Context *completion)
   : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                           cmp_bl.length(), snapc,
                                           "compare_and_write", parent_trace,
                                           completion),
    m_cmp_bl(std::move(cmp_bl)), m_write_bl(std::move(write_bl)),
    m_mismatch_offset(mismatch_offset), m_op_flags(op_flags) {
  }

  const char *get_op_type() const override {
    return "compare_and_write";
  }

  void add_copyup_ops(librados::ObjectWriteOperation *wr) override {
    // no-op on copyup
  }

protected:
  virtual bool is_post_copyup_write_required() const {
    return true;
  }

  void add_write_ops(librados::ObjectWriteOperation *wr) override;

  int filter_write_result(int r) const override;

private:
  ceph::bufferlist m_cmp_bl;
  ceph::bufferlist m_write_bl;
  uint64_t *m_mismatch_offset;
  int m_op_flags;
};

} // namespace io
} // namespace librbd

extern template class librbd::io::ObjectRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectReadRequest<librbd::ImageCtx>;
extern template class librbd::io::AbstractObjectWriteRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectWriteRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectDiscardRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectWriteSameRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectCompareAndWriteRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IO_OBJECT_REQUEST_H
