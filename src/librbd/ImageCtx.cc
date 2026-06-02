// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include <errno.h>
#include <boost/assign/list_of.hpp>
#include <stddef.h>

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/perf_counters.h"
#include "common/strtol.h"
#include "common/WorkQueue.h"
#include "common/Timer.h"

#include "librbd/AsyncRequest.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/internal.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/ImageWatcher.h"
#include "librbd/Journal.h"
#include "librbd/LibrbdAdminSocketHook.h"
#include "librbd/ObjectMap.h"
#include "librbd/Operations.h"
#include "librbd/operation/ResizeRequest.h"
#include "librbd/Types.h"
#include "librbd/Utils.h"
#include "librbd/LibrbdWriteback.h"
#include "librbd/exclusive_lock/AutomaticPolicy.h"
#include "librbd/exclusive_lock/StandardPolicy.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/AsyncOperation.h"
#include "librbd/io/AsyncWritebackThrottler.h"
#include "librbd/io/ImageRequestWQ.h"
#include "librbd/io/ObjectDispatcher.h"
#include "librbd/journal/StandardPolicy.h"

#include "osdc/Striper.h"
#include <boost/bind.hpp>
#include <boost/algorithm/string/predicate.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::ImageCtx: "

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

using ceph::bufferlist;
using librados::snap_t;
using librados::IoCtx;

namespace librbd {

namespace {

class ThreadPoolSingleton : public ThreadPool {
public:
  ContextWQ *op_work_queue;

  explicit ThreadPoolSingleton(CephContext *cct)
    : ThreadPool(cct, "librbd::thread_pool", "tp_librbd", 1,
                 "rbd_op_threads"),
      op_work_queue(new ContextWQ("librbd::op_work_queue",
                                  cct->_conf.get_val<uint64_t>("rbd_op_thread_timeout"),
                                  this)) {
    start();
  }
  ~ThreadPoolSingleton() override {
    op_work_queue->drain();
    delete op_work_queue;

    stop();
  }
};

class SafeTimerSingleton : public SafeTimer {
public:
  Mutex lock;

  explicit SafeTimerSingleton(CephContext *cct)
      : SafeTimer(cct, lock, true),
        lock("librbd::Journal::SafeTimerSingleton::lock") {
    init();
  }
  ~SafeTimerSingleton() {
    Mutex::Locker locker(lock);
    shutdown();
  }
};

} // anonymous namespace

  const string ImageCtx::METADATA_CONF_PREFIX = "conf_";

  ImageCtx::ImageCtx(const string &image_name, const string &image_id,
		     const char *snap, IoCtx& p, bool ro)
    : cct((CephContext*)p.cct()),
      config(cct->_conf),
      perfcounter(NULL),
      snap_id(CEPH_NOSNAP),
      snap_exists(true),
      read_only(ro),
      exclusive_locked(false),
      name(image_name),
      image_watcher(NULL),
      journal(NULL),
      owner_lock(util::unique_lock_name("librbd::ImageCtx::owner_lock", this)),
      md_lock(util::unique_lock_name("librbd::ImageCtx::md_lock", this)),
      snap_lock(util::unique_lock_name("librbd::ImageCtx::snap_lock", this)),
      timestamp_lock(util::unique_lock_name("librbd::ImageCtx::timestamp_lock", this)),
      parent_lock(util::unique_lock_name("librbd::ImageCtx::parent_lock", this)),
      object_map_lock(util::unique_lock_name("librbd::ImageCtx::object_map_lock", this)),
      async_ops_lock(util::unique_lock_name("librbd::ImageCtx::async_ops_lock", this)),
      copyup_list_lock(util::unique_lock_name("librbd::ImageCtx::copyup_list_lock", this)),
      completed_reqs_lock(util::unique_lock_name("librbd::ImageCtx::completed_reqs_lock", this)),
      known_zero_lock(util::unique_lock_name("librbd::ImageCtx::known_zero_lock", this)),
      pending_backfill_lock(util::unique_lock_name("librbd::ImageCtx::pending_backfill_lock", this)),
      extra_read_flags(0),
      old_format(false),
      order(0), size(0), features(0),
      format_string(NULL),
      id(image_id), parent(NULL),
      stripe_unit(0), stripe_count(0), flags(0),
      readahead(),
      total_bytes_read(0),
      state(new ImageState<>(this)),
      operations(new Operations<>(*this)),
      exclusive_lock(nullptr), object_map(nullptr),
      io_work_queue(nullptr), op_work_queue(nullptr),
      asok_hook(nullptr),
      trace_endpoint("librbd")
  {
    md_ctx.dup(p);
    data_ctx.dup(p);
    if (snap)
      snap_name = snap;

    // FIPS zeroization audit 20191117: this memset is not security related.
    memset(&header, 0, sizeof(header));

    ThreadPool *thread_pool;
    get_thread_pool_instance(cct, &thread_pool, &op_work_queue);
    io_work_queue = new io::ImageRequestWQ<>(
      this, "librbd::io_work_queue",
      cct->_conf.get_val<uint64_t>("rbd_op_thread_timeout"),
      thread_pool);
    io_object_dispatcher = new io::ObjectDispatcher<>(this);

    if (cct->_conf.get_val<bool>("rbd_auto_exclusive_lock_until_manual_request")) {
      exclusive_lock_policy = new exclusive_lock::AutomaticPolicy(this);
    } else {
      exclusive_lock_policy = new exclusive_lock::StandardPolicy(this);
    }
    journal_policy = new journal::StandardPolicy<ImageCtx>(this);
  }

  ImageCtx::ImageCtx(const string &image_name, const string &image_id,
		     uint64_t snap_id, IoCtx& p, bool ro)
    : ImageCtx(image_name, image_id, "", p, ro) {
    open_snap_id = snap_id;
  }

  ImageCtx::~ImageCtx() {
    ceph_assert(image_watcher == NULL);
    ceph_assert(exclusive_lock == NULL);
    ceph_assert(object_map == NULL);
    ceph_assert(journal == NULL);
    ceph_assert(asok_hook == NULL);

    if (perfcounter) {
      perf_stop();
    }
    delete[] format_string;

    // Wait for any detached parent-cache writebacks fired by
    // ObjectReadRequest (and CopyupRequest, in c3) through the
    // AsyncWritebackThrottler.  The throttler holds COPIES of our
    // data_ctx that aio_flush below cannot drain on its own — those
    // copies' state machines must terminate BEFORE we tear down the
    // owning librados client (rbd CLI invocations are the typical
    // case: rbd export/bench finishes, librados shuts down within
    // microseconds, and any still-pending throttler op would
    // aio_operate on a destroyed IoCtxImpl, silently dropping or
    // segfaulting).  Without this drain, the parent's RADOS cache
    // never gets populated for short-lived CLI clients and the
    // backfill-suite parent_du_after_child_writeback test caught
    // exactly this regression.
    io::AsyncWritebackThrottler::instance(cct).wait_for_idle();

    md_ctx.aio_flush();
    if (data_ctx.is_valid()) {
      data_ctx.aio_flush();
    }
    io_work_queue->drain();

    delete io_object_dispatcher;

    delete journal_policy;
    delete exclusive_lock_policy;
    delete io_work_queue;
    delete operations;
    delete state;

    // Shutdown remote parent cluster connection if present
    // This prevents resource leaks (network connections, messenger threads)
    if (remote_parent_cluster) {
      remote_parent_cluster->shutdown();
      remote_parent_cluster.reset();
    }
  }

  void ImageCtx::init() {
    ceph_assert(!header_oid.empty());
    ceph_assert(old_format || !id.empty());

    asok_hook = new LibrbdAdminSocketHook(this);

    string pname = string("librbd-") + id + string("-") +
      md_ctx.get_pool_name() + string("-") + name;
    if (!snap_name.empty()) {
      pname += "-";
      pname += snap_name;
    }

    trace_endpoint.copy_name(pname);
    perf_start(pname);

    ceph_assert(image_watcher == NULL);
    image_watcher = new ImageWatcher<>(*this);
  }

  void ImageCtx::shutdown() {
    delete image_watcher;
    image_watcher = nullptr;

    delete asok_hook;
    asok_hook = nullptr;
  }

  void ImageCtx::init_layout(int64_t pool_id)
  {
    if (stripe_unit == 0 || stripe_count == 0) {
      stripe_unit = 1ull << order;
      stripe_count = 1;
    }

    vector<uint64_t> alignments;
    alignments.push_back(stripe_count << order); // object set (in file striping terminology)
    alignments.push_back(stripe_unit * stripe_count); // stripe
    alignments.push_back(stripe_unit); // stripe unit
    readahead.set_alignments(alignments);

    layout = file_layout_t();
    layout.stripe_unit = stripe_unit;
    layout.stripe_count = stripe_count;
    layout.object_size = 1ull << order;
    layout.pool_id = pool_id;  // FIXME: pool id overflow?

    delete[] format_string;
    size_t len = object_prefix.length() + 16;
    format_string = new char[len];
    if (old_format) {
      snprintf(format_string, len, "%s.%%012llx", object_prefix.c_str());
    } else {
      snprintf(format_string, len, "%s.%%016llx", object_prefix.c_str());
    }

    ldout(cct, 10) << "init_layout stripe_unit " << stripe_unit
		   << " stripe_count " << stripe_count
		   << " object_size " << layout.object_size
		   << " prefix " << object_prefix
		   << " format " << format_string
		   << dendl;
  }

  void ImageCtx::perf_start(string name) {
    auto perf_prio = PerfCountersBuilder::PRIO_DEBUGONLY;
    if (child == nullptr) {
      // ensure top-level IO stats are exported for librbd daemons
      perf_prio = PerfCountersBuilder::PRIO_USEFUL;
    }

    PerfCountersBuilder plb(cct, name, l_librbd_first, l_librbd_last);

    plb.add_u64_counter(l_librbd_rd, "rd", "Reads", "r", perf_prio);
    plb.add_u64_counter(l_librbd_rd_bytes, "rd_bytes", "Data size in reads",
                        "rb", perf_prio, unit_t(UNIT_BYTES));
    plb.add_time_avg(l_librbd_rd_latency, "rd_latency", "Latency of reads",
                     "rl", perf_prio);
    plb.add_u64_counter(l_librbd_wr, "wr", "Writes", "w", perf_prio);
    plb.add_u64_counter(l_librbd_wr_bytes, "wr_bytes", "Written data",
                        "wb", perf_prio, unit_t(UNIT_BYTES));
    plb.add_time_avg(l_librbd_wr_latency, "wr_latency", "Write latency",
                     "wl", perf_prio);
    plb.add_u64_counter(l_librbd_discard, "discard", "Discards");
    plb.add_u64_counter(l_librbd_discard_bytes, "discard_bytes", "Discarded data", NULL, 0, unit_t(UNIT_BYTES));
    plb.add_time_avg(l_librbd_discard_latency, "discard_latency", "Discard latency");
    plb.add_u64_counter(l_librbd_flush, "flush", "Flushes");
    plb.add_time_avg(l_librbd_flush_latency, "flush_latency", "Latency of flushes");
    plb.add_u64_counter(l_librbd_ws, "ws", "WriteSames");
    plb.add_u64_counter(l_librbd_ws_bytes, "ws_bytes", "WriteSame data", NULL, 0, unit_t(UNIT_BYTES));
    plb.add_time_avg(l_librbd_ws_latency, "ws_latency", "WriteSame latency");
    plb.add_u64_counter(l_librbd_cmp, "cmp", "CompareAndWrites");
    plb.add_u64_counter(l_librbd_cmp_bytes, "cmp_bytes", "Data size in cmps", NULL, 0, unit_t(UNIT_BYTES));
    plb.add_time_avg(l_librbd_cmp_latency, "cmp_latency", "Latency of cmps");
    plb.add_u64_counter(l_librbd_snap_create, "snap_create", "Snap creations");
    plb.add_u64_counter(l_librbd_snap_remove, "snap_remove", "Snap removals");
    plb.add_u64_counter(l_librbd_snap_rollback, "snap_rollback", "Snap rollbacks");
    plb.add_u64_counter(l_librbd_snap_rename, "snap_rename", "Snap rename");
    plb.add_u64_counter(l_librbd_notify, "notify", "Updated header notifications");
    plb.add_u64_counter(l_librbd_resize, "resize", "Resizes");
    plb.add_u64_counter(l_librbd_readahead, "readahead", "Read ahead");
    plb.add_u64_counter(l_librbd_readahead_bytes, "readahead_bytes", "Data size in read ahead", NULL, 0, unit_t(UNIT_BYTES));
    plb.add_u64_counter(l_librbd_invalidate_cache, "invalidate_cache", "Cache invalidates");

    // S3-backed parent performance counters
    plb.add_u64_counter(l_librbd_s3_fetch_count, "s3_fetch_count",
                        "S3 fetch operations", "s3fc", perf_prio);
    plb.add_u64_counter(l_librbd_s3_fetch_bytes, "s3_fetch_bytes",
                        "Bytes fetched from S3", "s3fb", perf_prio, unit_t(UNIT_BYTES));
    plb.add_u64_counter(l_librbd_s3_fetch_errors, "s3_fetch_errors",
                        "S3 fetch errors", "s3fe", perf_prio);

    plb.add_time(l_librbd_opened_time, "opened_time", "Opened time",
                 "ots", perf_prio);
    plb.add_time(l_librbd_lock_acquired_time, "lock_acquired_time",
                 "Lock acquired time", "lats", perf_prio);

    perfcounter = plb.create_perf_counters();
    cct->get_perfcounters_collection()->add(perfcounter);

    perfcounter->tset(l_librbd_opened_time, ceph_clock_now());
  }

  void ImageCtx::perf_stop() {
    ceph_assert(perfcounter);
    cct->get_perfcounters_collection()->remove(perfcounter);
    delete perfcounter;
  }

  void ImageCtx::set_read_flag(unsigned flag) {
    extra_read_flags |= flag;
  }

  int ImageCtx::get_read_flags(snap_t snap_id) {
    int flags = librados::OPERATION_NOFLAG | extra_read_flags;
    if (snap_id == LIBRADOS_SNAP_HEAD)
      return flags;

    if (config.get_val<bool>("rbd_balance_snap_reads"))
      flags |= librados::OPERATION_BALANCE_READS;
    else if (config.get_val<bool>("rbd_localize_snap_reads"))
      flags |= librados::OPERATION_LOCALIZE_READS;
    return flags;
  }

  int ImageCtx::snap_set(uint64_t in_snap_id) {
    ceph_assert(snap_lock.is_wlocked());
    auto it = snap_info.find(in_snap_id);
    if (in_snap_id != CEPH_NOSNAP && it != snap_info.end()) {
      snap_id = in_snap_id;
      snap_namespace = it->second.snap_namespace;
      snap_name = it->second.name;
      snap_exists = true;
      if (data_ctx.is_valid()) {
        data_ctx.snap_set_read(snap_id);
      }
      return 0;
    }
    return -ENOENT;
  }

  void ImageCtx::snap_unset()
  {
    ceph_assert(snap_lock.is_wlocked());
    snap_id = CEPH_NOSNAP;
    snap_namespace = {};
    snap_name = "";
    snap_exists = true;
    if (data_ctx.is_valid()) {
      data_ctx.snap_set_read(snap_id);
    }
  }

  snap_t ImageCtx::get_snap_id(const cls::rbd::SnapshotNamespace& in_snap_namespace,
                               const string& in_snap_name) const
  {
    ceph_assert(snap_lock.is_locked());
    auto it = snap_ids.find({in_snap_namespace, in_snap_name});
    if (it != snap_ids.end()) {
      return it->second;
    }
    return CEPH_NOSNAP;
  }

  const SnapInfo* ImageCtx::get_snap_info(snap_t in_snap_id) const
  {
    ceph_assert(snap_lock.is_locked());
    map<snap_t, SnapInfo>::const_iterator it =
      snap_info.find(in_snap_id);
    if (it != snap_info.end())
      return &it->second;
    return nullptr;
  }

  int ImageCtx::get_snap_name(snap_t in_snap_id,
			      string *out_snap_name) const
  {
    ceph_assert(snap_lock.is_locked());
    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info) {
      *out_snap_name = info->name;
      return 0;
    }
    return -ENOENT;
  }

  int ImageCtx::get_snap_namespace(snap_t in_snap_id,
				   cls::rbd::SnapshotNamespace *out_snap_namespace) const
  {
    ceph_assert(snap_lock.is_locked());
    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info) {
      *out_snap_namespace = info->snap_namespace;
      return 0;
    }
    return -ENOENT;
  }

  int ImageCtx::get_parent_spec(snap_t in_snap_id,
				cls::rbd::ParentImageSpec *out_pspec) const
  {
    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info) {
      *out_pspec = info->parent.spec;
      return 0;
    }
    return -ENOENT;
  }

  uint64_t ImageCtx::get_current_size() const
  {
    ceph_assert(snap_lock.is_locked());
    return size;
  }

  uint64_t ImageCtx::get_object_size() const
  {
    return 1ull << order;
  }

  string ImageCtx::get_object_name(uint64_t num) const {
    char buf[object_prefix.length() + 32];
    snprintf(buf, sizeof(buf), format_string, num);
    return string(buf);
  }

  uint64_t ImageCtx::get_stripe_unit() const
  {
    return stripe_unit;
  }

  uint64_t ImageCtx::get_stripe_count() const
  {
    return stripe_count;
  }

  uint64_t ImageCtx::get_stripe_period() const
  {
    return stripe_count * (1ull << order);
  }

  utime_t ImageCtx::get_create_timestamp() const
  {
    return create_timestamp;
  }

  utime_t ImageCtx::get_access_timestamp() const
  {
    return access_timestamp;
  }

  utime_t ImageCtx::get_modify_timestamp() const
  {
    return modify_timestamp;
  }

  void ImageCtx::set_access_timestamp(utime_t at)
  {
    ceph_assert(timestamp_lock.is_wlocked());
    access_timestamp = at;
  }

  void ImageCtx::set_modify_timestamp(utime_t mt)
  {
    ceph_assert(timestamp_lock.is_locked());
    modify_timestamp = mt;
  }

  int ImageCtx::is_snap_protected(snap_t in_snap_id,
				  bool *is_protected) const
  {
    ceph_assert(snap_lock.is_locked());
    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info) {
      *is_protected =
	(info->protection_status == RBD_PROTECTION_STATUS_PROTECTED);
      return 0;
    }
    return -ENOENT;
  }

  int ImageCtx::is_snap_unprotected(snap_t in_snap_id,
				    bool *is_unprotected) const
  {
    ceph_assert(snap_lock.is_locked());
    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info) {
      *is_unprotected =
	(info->protection_status == RBD_PROTECTION_STATUS_UNPROTECTED);
      return 0;
    }
    return -ENOENT;
  }

  void ImageCtx::add_snap(cls::rbd::SnapshotNamespace in_snap_namespace,
			  string in_snap_name,
			  snap_t id, uint64_t in_size,
			  const ParentImageInfo &parent,
                          uint8_t protection_status, uint64_t flags,
                          utime_t timestamp)
  {
    ceph_assert(snap_lock.is_wlocked());
    snaps.push_back(id);
    SnapInfo info(in_snap_name, in_snap_namespace,
		  in_size, parent, protection_status, flags, timestamp);
    snap_info.insert({id, info});
    snap_ids.insert({{in_snap_namespace, in_snap_name}, id});
  }

  void ImageCtx::rm_snap(cls::rbd::SnapshotNamespace in_snap_namespace,
			 string in_snap_name,
			 snap_t id)
  {
    ceph_assert(snap_lock.is_wlocked());
    snaps.erase(std::remove(snaps.begin(), snaps.end(), id), snaps.end());
    snap_info.erase(id);
    snap_ids.erase({in_snap_namespace, in_snap_name});
  }

  uint64_t ImageCtx::get_image_size(snap_t in_snap_id) const
  {
    ceph_assert(snap_lock.is_locked());
    if (in_snap_id == CEPH_NOSNAP) {
      if (!resize_reqs.empty() &&
          resize_reqs.front()->shrinking()) {
        return resize_reqs.front()->get_image_size();
      }
      return size;
    }

    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info) {
      return info->size;
    }
    return 0;
  }

  uint64_t ImageCtx::get_object_count(snap_t in_snap_id) const {
    ceph_assert(snap_lock.is_locked());
    uint64_t image_size = get_image_size(in_snap_id);
    return Striper::get_num_objects(layout, image_size);
  }

  bool ImageCtx::test_features(uint64_t features) const
  {
    RWLock::RLocker l(snap_lock);
    return test_features(features, snap_lock);
  }

  bool ImageCtx::test_features(uint64_t in_features,
                               const RWLock &in_snap_lock) const
  {
    ceph_assert(snap_lock.is_locked());
    return ((features & in_features) == in_features);
  }

  bool ImageCtx::test_op_features(uint64_t in_op_features) const
  {
    RWLock::RLocker snap_locker(snap_lock);
    return test_op_features(in_op_features, snap_lock);
  }

  bool ImageCtx::test_op_features(uint64_t in_op_features,
                                  const RWLock &in_snap_lock) const
  {
    ceph_assert(snap_lock.is_locked());
    return ((op_features & in_op_features) == in_op_features);
  }

  int ImageCtx::get_flags(librados::snap_t _snap_id, uint64_t *_flags) const
  {
    ceph_assert(snap_lock.is_locked());
    if (_snap_id == CEPH_NOSNAP) {
      *_flags = flags;
      return 0;
    }
    const SnapInfo *info = get_snap_info(_snap_id);
    if (info) {
      *_flags = info->flags;
      return 0;
    }
    return -ENOENT;
  }

  int ImageCtx::test_flags(librados::snap_t in_snap_id,
                           uint64_t flags, bool *flags_set) const
  {
    RWLock::RLocker l(snap_lock);
    return test_flags(in_snap_id, flags, snap_lock, flags_set);
  }

  int ImageCtx::test_flags(librados::snap_t in_snap_id,
                           uint64_t flags, const RWLock &in_snap_lock,
                           bool *flags_set) const
  {
    ceph_assert(snap_lock.is_locked());
    uint64_t snap_flags;
    int r = get_flags(in_snap_id, &snap_flags);
    if (r < 0) {
      return r;
    }
    *flags_set = ((snap_flags & flags) == flags);
    return 0;
  }

  int ImageCtx::update_flags(snap_t in_snap_id, uint64_t flag, bool enabled)
  {
    ceph_assert(snap_lock.is_wlocked());
    uint64_t *_flags;
    if (in_snap_id == CEPH_NOSNAP) {
      _flags = &flags;
    } else {
      map<snap_t, SnapInfo>::iterator it = snap_info.find(in_snap_id);
      if (it == snap_info.end()) {
        return -ENOENT;
      }
      _flags = &it->second.flags;
    }

    if (enabled) {
      (*_flags) |= flag;
    } else {
      (*_flags) &= ~flag;
    }
    return 0;
  }

  const ParentImageInfo* ImageCtx::get_parent_info(snap_t in_snap_id) const
  {
    ceph_assert(snap_lock.is_locked());
    ceph_assert(parent_lock.is_locked());
    if (in_snap_id == CEPH_NOSNAP)
      return &parent_md;
    const SnapInfo *info = get_snap_info(in_snap_id);
    if (info)
      return &info->parent;
    return NULL;
  }

  int64_t ImageCtx::get_parent_pool_id(snap_t in_snap_id) const
  {
    const auto info = get_parent_info(in_snap_id);
    if (info)
      return info->spec.pool_id;
    return -1;
  }

  string ImageCtx::get_parent_image_id(snap_t in_snap_id) const
  {
    const auto info = get_parent_info(in_snap_id);
    if (info)
      return info->spec.image_id;
    return "";
  }

  uint64_t ImageCtx::get_parent_snap_id(snap_t in_snap_id) const
  {
    const auto info = get_parent_info(in_snap_id);
    if (info)
      return info->spec.snap_id;
    return CEPH_NOSNAP;
  }

  int ImageCtx::get_parent_overlap(snap_t in_snap_id, uint64_t *overlap) const
  {
    ceph_assert(snap_lock.is_locked());
    const auto info = get_parent_info(in_snap_id);
    if (info) {
      *overlap = info->overlap;
      return 0;
    }
    return -ENOENT;
  }

  void ImageCtx::register_watch(Context *on_finish) {
    ceph_assert(image_watcher != NULL);
    image_watcher->register_watch(on_finish);
  }

  uint64_t ImageCtx::prune_parent_extents(vector<pair<uint64_t,uint64_t> >& objectx,
					  uint64_t overlap)
  {
    // drop extents completely beyond the overlap
    while (!objectx.empty() && objectx.back().first >= overlap)
      objectx.pop_back();

    // trim final overlapping extent
    if (!objectx.empty() && objectx.back().first + objectx.back().second > overlap)
      objectx.back().second = overlap - objectx.back().first;

    uint64_t len = 0;
    for (vector<pair<uint64_t,uint64_t> >::iterator p = objectx.begin();
	 p != objectx.end();
	 ++p)
      len += p->second;
    ldout(cct, 10) << "prune_parent_extents image overlap " << overlap
		   << ", object overlap " << len
		   << " from image extents " << objectx << dendl;
    return len;
  }

  void ImageCtx::cancel_async_requests() {
    C_SaferCond ctx;
    cancel_async_requests(&ctx);
    ctx.wait();
  }

  void ImageCtx::cancel_async_requests(Context *on_finish) {
    {
      Mutex::Locker async_ops_locker(async_ops_lock);
      if (!async_requests.empty()) {
        ldout(cct, 10) << "canceling async requests: count="
                       << async_requests.size() << dendl;
        for (auto req : async_requests) {
          ldout(cct, 10) << "canceling async request: " << req << dendl;
          req->cancel();
        }
        async_requests_waiters.push_back(on_finish);
        return;
      }
    }

    on_finish->complete(0);
  }

  void ImageCtx::clear_pending_completions() {
    Mutex::Locker l(completed_reqs_lock);
    ldout(cct, 10) << "clear pending AioCompletion: count="
                   << completed_reqs.size() << dendl;
    completed_reqs.clear();
  }

  void ImageCtx::apply_metadata(const std::map<std::string, bufferlist> &meta,
                                bool thread_safe) {
    ldout(cct, 20) << __func__ << dendl;

    // reset settings back to global defaults
    for (auto& key : config_overrides) {
      std::string value;
      int r = cct->_conf.get_val(key, &value);
      ceph_assert(r == 0);

      config.set_val(key, value);
    }
    config_overrides.clear();

    // extract config overrides
    for (const auto& meta_pair : meta) {
      if (!boost::starts_with(meta_pair.first, METADATA_CONF_PREFIX)) {
        continue;
      }

      std::string key = meta_pair.first.substr(METADATA_CONF_PREFIX.size());
      if (!boost::starts_with(key, "rbd_")) {
        // ignore non-RBD configuration keys
        // TODO use option schema to determine applicable subsystem
        ldout(cct, 0) << __func__ << ": ignoring config " << key << dendl;
        continue;
      }

      if (config.find_option(key) != nullptr) {
        std::string val = meta_pair.second.to_str();
        int r = config.set_val(key, val);
        if (r >= 0) {
          ldout(cct, 20) << __func__ << ": " << key << "=" << val << dendl;
          config_overrides.insert(key);
        } else {
          lderr(cct) << __func__ << ": failed to set config " << key << " "
                     << "with value " << val << ": " << cpp_strerror(r)
                     << dendl;
        }
      }
    }

#define ASSIGN_OPTION(param, type)              \
    param = config.get_val<type>("rbd_"#param)

    bool skip_partial_discard = true;
    ASSIGN_OPTION(non_blocking_aio, bool);
    ASSIGN_OPTION(cache, bool);
    ASSIGN_OPTION(cache_writethrough_until_flush, bool);
    ASSIGN_OPTION(cache_max_dirty, Option::size_t);
    ASSIGN_OPTION(sparse_read_threshold_bytes, Option::size_t);
    ASSIGN_OPTION(readahead_max_bytes, Option::size_t);
    ASSIGN_OPTION(readahead_disable_after_bytes, Option::size_t);
    ASSIGN_OPTION(clone_copy_on_read, bool);
    ASSIGN_OPTION(enable_alloc_hint, bool);
    ASSIGN_OPTION(mirroring_replay_delay, uint64_t);
    ASSIGN_OPTION(mtime_update_interval, uint64_t);
    ASSIGN_OPTION(atime_update_interval, uint64_t);
    ASSIGN_OPTION(skip_partial_discard, bool);
    ASSIGN_OPTION(discard_granularity_bytes, uint64_t);
    ASSIGN_OPTION(blkin_trace_all, bool);

#undef ASSIGN_OPTION

    if (sparse_read_threshold_bytes == 0) {
      sparse_read_threshold_bytes = get_object_size();
    }
    if (!skip_partial_discard) {
      discard_granularity_bytes = 0;
    }

    // Cache S3-related config options to avoid per-I/O config reads
    s3_fetch_enabled = cct->_conf.template get_val<bool>("rbd_s3_fetch_enabled");
    s3_parent_lock_timeout = static_cast<uint32_t>(
      cct->_conf.template get_val<uint64_t>("rbd_s3_parent_lock_timeout"));
    s3_lock_retry_max = static_cast<uint32_t>(
      cct->_conf.template get_val<uint64_t>("rbd_s3_lock_retry_max"));

    // Load S3 configuration from image metadata
    s3_config = S3Config();  // Reset to defaults first
    bool s3_secret_key_invalid = false;
    bool s3_enabled_explicitly_set = false;

    // Helper: parse a non-negative integer metadata value into a uint32.
    auto parse_uint32_meta = [this](const std::string& val,
                                    const char* key_name,
                                    uint32_t* out) {
      std::string err;
      long long v = strict_strtoll(val.c_str(), 10, &err);
      if (!err.empty() || v < 0) {
        lderr(cct) << "apply_metadata: invalid " << key_name
                   << " value: " << val << dendl;
      } else {
        *out = static_cast<uint32_t>(v);
      }
    };

    for (const auto& meta_pair : meta) {
      const std::string& k = meta_pair.first;
      if (!boost::starts_with(k, librbd::S3_META_NS)) {
        continue;
      }

      std::string val = meta_pair.second.to_str();

      if (k == S3_META_KEY_ENABLED) {
        s3_enabled_explicitly_set = true;
        s3_config.enabled = (val == "true" || val == "1");
      } else if (k == S3_META_KEY_ENDPOINT) {
        s3_config.endpoint = val;
      } else if (k == S3_META_KEY_BUCKET) {
        s3_config.bucket = val;
      } else if (k == S3_META_KEY_REGION) {
        s3_config.region = val;
      } else if (k == S3_META_KEY_ACCESS_KEY) {
        s3_config.access_key = val;
      } else if (k == S3_META_KEY_SECRET_KEY) {
        // The metadata value is stored base64-encoded (set by `rbd s3-config set`).
        // Decode it here so HMAC signing uses the raw key, not the encoded form.
        // Note: metadata is sorted, so access_key ('a') is processed before
        // secret_key ('s') — s3_config.access_key is already set if present.
        try {
          bufferlist encoded_bl;
          encoded_bl.append(val);
          bufferlist decoded_bl;
          decoded_bl.decode_base64(encoded_bl);
          s3_config.secret_key = decoded_bl.to_str();
        } catch (const buffer::error&) {
          if (!s3_config.access_key.empty()) {
            // access_key is set but secret_key is not valid base64 — this is
            // a credential configuration error; S3 will not be enabled.
            s3_secret_key_invalid = true;
          }
          // If access_key is empty, the config is incomplete anyway; do nothing.
        }
      } else if (k == S3_META_KEY_IMAGE_NAME) {
        s3_config.image_name = val;
      } else if (k == S3_META_KEY_IMAGE_FMT) {
        s3_config.image_format = val;
      } else if (k == S3_META_KEY_PREFIX) {
        s3_config.prefix = val;
      } else if (k == S3_META_KEY_TIMEOUT_MS) {
        parse_uint32_meta(val, S3_META_KEY_TIMEOUT_MS, &s3_config.timeout_ms);
      } else if (k == S3_META_KEY_MAX_RETRIES) {
        parse_uint32_meta(val, S3_META_KEY_MAX_RETRIES, &s3_config.max_retries);
      }
    }

    if (s3_secret_key_invalid) {
      lderr(cct) << __func__ << ": CRITICAL: failed to decode s3.secret_key for "
                 << "authenticated access. S3 configuration is invalid - all reads "
                 << "from this parent will fail. Please check the s3.secret_key "
                 << "metadata on the parent image." << dendl;
      // Leave s3_config disabled — do not fall through to auto-enable.
    } else
    // Auto-enable S3 if required fields are present and the user did not
    // explicitly set s3.enabled=false.  An explicit "false" is an escape hatch
    // for operators who want to disable S3 reads without removing the metadata.
    if (!s3_enabled_explicitly_set &&
        !s3_config.bucket.empty() && !s3_config.endpoint.empty() &&
        !s3_config.image_name.empty() && !s3_config.image_format.empty()) {
      s3_config.enabled = true;
      ldout(cct, 10) << __func__ << ": S3 config loaded: endpoint=" << s3_config.endpoint
                     << ", bucket=" << s3_config.bucket
                     << ", image_name=" << s3_config.image_name
                     << ", format=" << s3_config.image_format << dendl;
    }

    // Track whether rbd-backfill has fully populated this image's RADOS
    // pool.  When complete, ObjectReadRequest::read_object can trust the
    // object_map: a NONEXISTENT bit means "known-zero, return ENOENT
    // without an S3 fetch" instead of "unknown, fall back to S3".  Look
    // up the key explicitly (cheap O(log N) on the already-fetched map);
    // we can't reuse the s3_config loop above because that loop filters
    // on the s3. prefix.  Keep the string literal in sync with
    // rbd::backfill::BACKFILL_STATUS_KEY / BACKFILL_STATUS_COMPLETE in
    // src/tools/rbd_backfill/Types.h (librbd cannot include tool-tree
    // headers).
    auto backfill_status_it = meta.find("backfill_status");
    if (backfill_status_it != meta.end()) {
      const std::string val = backfill_status_it->second.to_str();
      s3_backfill_complete = (val == "complete");
      ldout(cct, 10) << __func__ << ": backfill_status=" << val
                     << " (s3_backfill_complete=" << s3_backfill_complete
                     << ")" << dendl;
    } else {
      s3_backfill_complete = false;
    }

    // Try to load the per-image backfill-visited bitmap (rbd_backfill_visited.<id>).
    // Always attempt for S3-backed images, not just when backfill_status is set:
    // a previously-canceled backfill may have left VISITED_ZERO bits that are
    // still safe to trust.  ENOENT -> no bitmap exists, reader falls through
    // to the pre-bitmap behavior.  Keep the OID prefix in sync with
    // RBD_BACKFILL_VISITED_PREFIX in src/include/rbd_types.h.
    // Load the per-image backfill-visited bitmap synchronously here so the
    // very first parent read after open can consult it without an extra
    // round-trip.  Trade-off: apply_metadata may be invoked from a librados
    // AIO finisher thread (RefreshRequest.cc:646), where a blocking GET
    // stalls other completions for the OSD round-trip.  Acceptable here
    // because the bitmap object is small (~64 KB per TB of image) and the
    // typical GET is sub-ms; only a degraded OSD on the bitmap object
    // produces a noticeable stall.  Periodic refreshes during the
    // ImageCtx's lifetime go via maybe_reload_backfill_visited(), which
    // uses async aio_operate to keep the finisher unblocked there.
    //
    // Pool: md_ctx (primary/metadata pool), matching where
    // ImageBackfiller::init_backfill_visited_bitmap and
    // RemoveRequest::send_backfill_visited_remove write/delete the
    // bitmap.  data_ctx would be wrong for --data-pool images.
    if (s3_config.is_valid()) {
      const std::string visited_oid =
        std::string(RBD_BACKFILL_VISITED_PREFIX) + id;
      auto loaded = std::make_shared<ceph::BitVector<2>>();
      int r = cls_client::object_map_load(&md_ctx, visited_oid, loaded.get());
      RWLock::WLocker md_locker(md_lock);
      if (r == 0) {
        std::atomic_store(&backfill_visited, loaded);
        ldout(cct, 10) << __func__ << ": loaded backfill-visited bitmap from "
                       << visited_oid << " (size=" << loaded->size() << ")"
                       << dendl;
      } else {
        std::atomic_store(&backfill_visited,
                          std::shared_ptr<ceph::BitVector<2>>{});
        if (r != -ENOENT) {
          ldout(cct, 5) << __func__ << ": failed to load backfill-visited "
                        << "bitmap from " << visited_oid << ": "
                        << cpp_strerror(r) << " -- proceeding without it"
                        << dendl;
        }
      }
      backfill_visited_loaded_at_sec.store(ceph_clock_now().sec(),
                                           std::memory_order_release);
    } else {
      RWLock::WLocker md_locker(md_lock);
      std::atomic_store(&backfill_visited,
                        std::shared_ptr<ceph::BitVector<2>>{});
      backfill_visited_loaded_at_sec.store(ceph_clock_now().sec(),
                                           std::memory_order_release);
    }

    alloc_hint_flags = 0;
    auto compression_hint = config.get_val<std::string>("rbd_compression_hint");
    if (compression_hint == "compressible") {
      alloc_hint_flags |= librados::ALLOC_HINT_FLAG_COMPRESSIBLE;
    } else if (compression_hint == "incompressible") {
      alloc_hint_flags |= librados::ALLOC_HINT_FLAG_INCOMPRESSIBLE;
    }

    io_work_queue->apply_qos_schedule_tick_min(
      config.get_val<uint64_t>("rbd_qos_schedule_tick_min"));

    io_work_queue->apply_qos_limit(
      RBD_QOS_IOPS_THROTTLE,
      config.get_val<uint64_t>("rbd_qos_iops_limit"),
      config.get_val<uint64_t>("rbd_qos_iops_burst"));
    io_work_queue->apply_qos_limit(
      RBD_QOS_BPS_THROTTLE,
      config.get_val<uint64_t>("rbd_qos_bps_limit"),
      config.get_val<uint64_t>("rbd_qos_bps_burst"));
    io_work_queue->apply_qos_limit(
      RBD_QOS_READ_IOPS_THROTTLE,
      config.get_val<uint64_t>("rbd_qos_read_iops_limit"),
      config.get_val<uint64_t>("rbd_qos_read_iops_burst"));
    io_work_queue->apply_qos_limit(
      RBD_QOS_WRITE_IOPS_THROTTLE,
      config.get_val<uint64_t>("rbd_qos_write_iops_limit"),
      config.get_val<uint64_t>("rbd_qos_write_iops_burst"));
    io_work_queue->apply_qos_limit(
      RBD_QOS_READ_BPS_THROTTLE,
      config.get_val<uint64_t>("rbd_qos_read_bps_limit"),
      config.get_val<uint64_t>("rbd_qos_read_bps_burst"));
    io_work_queue->apply_qos_limit(
      RBD_QOS_WRITE_BPS_THROTTLE,
      config.get_val<uint64_t>("rbd_qos_write_bps_limit"),
      config.get_val<uint64_t>("rbd_qos_write_bps_burst"));
  }

  ExclusiveLock<ImageCtx> *ImageCtx::create_exclusive_lock() {
    return new ExclusiveLock<ImageCtx>(*this);
  }

  ObjectMap<ImageCtx> *ImageCtx::create_object_map(uint64_t snap_id) {
    return new ObjectMap<ImageCtx>(*this, snap_id);
  }

  void ImageCtx::maybe_reload_backfill_visited() {
    // s3_config is written by apply_metadata under no lock; take snap_lock
    // RLocker to make the is_valid() check race-free against a concurrent
    // RefreshRequest that's mid-population of the s3_config struct.  This
    // pairs with the WLocker apply_metadata takes around the bitmap block.
    {
      RWLock::RLocker snap_locker(snap_lock);
      if (!s3_config.is_valid()) {
        return;
      }
    }
    uint64_t interval =
      config.get_val<uint64_t>("rbd_s3_backfill_visited_reload_interval");
    if (interval == 0) {
      return;  // periodic reload disabled
    }

    uint64_t now_sec = static_cast<uint64_t>(ceph_clock_now().sec());
    uint64_t loaded_sec =
      backfill_visited_loaded_at_sec.load(std::memory_order_acquire);
    // loaded_sec == 0 on a fresh ImageCtx that hasn't loaded yet; the
    // subtraction yields ~now_sec, which is far above interval and
    // correctly triggers a reload.  Underflow-safe via signed compare
    // in the other direction: if loaded_sec > now_sec (clock skew),
    // treat as fresh and return.
    if (loaded_sec > now_sec || (now_sec - loaded_sec) < interval) {
      return;
    }

    // Set the in-flight flag under WLocker BEFORE submitting the aio.
    // This is the single point that bounds concurrent reloads to one: a
    // racing caller that also reaches this WLocker will see in_flight=true
    // and bail without submitting a redundant RADOS GET.
    {
      RWLock::WLocker md_locker(md_lock);
      if (backfill_visited_reload_in_flight) {
        return;
      }
      backfill_visited_reload_in_flight = true;
    }

    // Lifetime: [this]-capture in the callback is safe because ~ImageCtx
    // calls md_ctx.aio_flush() before tearing down (see ImageCtx.cc:192),
    // which blocks until all in-flight aios on md_ctx complete -- including
    // their callbacks.  This is why we use md_ctx and not op_work_queue
    // (which has no destructor-time drain for this ImageCtx).  md_ctx is
    // also the right pool for the bitmap: the daemon's
    // ImageBackfiller::init_backfill_visited_bitmap writes via the
    // primary/metadata pool too, and data_ctx may be retargeted away
    // (OpenRequest.cc:473-474 retargets data_ctx to the data pool for
    // --data-pool images, leaving md_ctx pointing at the primary pool).
    const std::string visited_oid =
      std::string(RBD_BACKFILL_VISITED_PREFIX) + id;
    auto* out_bl = new bufferlist();

    librados::ObjectReadOperation op;
    cls_client::object_map_load_start(&op);

    auto* on_finish = new FunctionContext(
      [this, out_bl](int load_r) {
        // Single in_flight = true / single submitted aio = one effective
        // bitmap GET per TTL interval per ImageCtx.  Subsequent racing
        // callers bail at the WLocker-guarded `if (in_flight) return`
        // above without queuing redundant aios.
        std::shared_ptr<ceph::BitVector<2>> new_bitmap;
        if (load_r == 0) {
          new_bitmap = std::make_shared<ceph::BitVector<2>>();
          auto it = out_bl->cbegin();
          int decode_r = cls_client::object_map_load_finish(&it,
                                                            new_bitmap.get());
          if (decode_r < 0) {
            ldout(cct, 5) << "maybe_reload_backfill_visited: decode failed: "
                          << cpp_strerror(decode_r) << dendl;
            new_bitmap.reset();
            load_r = decode_r;
          }
        }

        {
          // Single-critical-section update of (bitmap, timestamp,
          // in_flight) so a reader observes a consistent triple.
          RWLock::WLocker md_locker(md_lock);
          if (load_r == 0) {
            std::atomic_store(&backfill_visited, new_bitmap);
            ldout(cct, 15) << "maybe_reload_backfill_visited: refreshed "
                           << "(size=" << new_bitmap->size() << ")" << dendl;
          } else if (load_r == -ENOENT) {
            std::atomic_store(&backfill_visited,
                              std::shared_ptr<ceph::BitVector<2>>{});
          } else {
            ldout(cct, 5) << "maybe_reload_backfill_visited: load failed: "
                          << cpp_strerror(load_r) << " -- keeping stale copy"
                          << dendl;
          }
          // Always update the timestamp (#9): without this, a sustained
          // transient RADOS error (-ETIMEDOUT, -EIO) leaves the timestamp
          // stale and every parent-read miss re-enqueues a fresh reload.
          // On success we get the natural fresh timestamp; on error we
          // wait one full TTL interval before retrying.
          backfill_visited_loaded_at_sec.store(ceph_clock_now().sec(),
                                               std::memory_order_release);
          backfill_visited_reload_in_flight = false;
        }
        delete out_bl;
      });

    auto* rados_completion = util::create_rados_callback(on_finish);
    int r = md_ctx.aio_operate(visited_oid, rados_completion, &op, out_bl);
    ceph_assert(r == 0);
    rados_completion->release();
  }

  void ImageCtx::note_known_zero_object(uint64_t object_no) {
    // In-memory record: cheap, always available, ImageCtx-scoped.  This is the
    // fast path that collapses repeated zero re-fetches within this process --
    // subsequent reads of object_no short-circuit in read_object()'s pre-aio
    // check and handle_read_object()'s ENOENT branch without touching S3.
    {
      Mutex::Locker locker(known_zero_lock);
      known_zero_objects.insert(object_no);
    }

    // Persist to the on-disk backfill-visited bitmap IFF it already exists.
    // A non-null backfill_visited means apply_metadata (or a reload) last
    // observed the bitmap object on disk, so an object_map_update has a
    // target.  We deliberately do NOT create/resize the bitmap here: a
    // multi-reader create race could clobber bits, and creation is the
    // daemon's job (ImageBackfiller::init_backfill_visited_bitmap).  When no
    // bitmap exists the in-memory set above still delivers the in-process
    // benefit.
    auto visited = std::atomic_load(&backfill_visited);
    if (!visited) {
      return;
    }
    // Best-effort, fire-and-forget, ENOENT/EBUSY-tolerant -- mirrors the
    // daemon's ObjectBackfillRequest::mark_visited but with no callback, since
    // a failed bit update only costs one wasted S3 GET later (a reader on
    // another ImageCtx re-discovers the zero).  md_ctx is the bitmap's pool
    // (see apply_metadata + maybe_reload_backfill_visited); ~ImageCtx's
    // md_ctx.aio_flush() drains this op before teardown.
    const std::string visited_oid =
      std::string(RBD_BACKFILL_VISITED_PREFIX) + id;
    librados::ObjectWriteOperation op;
    cls_client::object_map_update(&op, object_no, object_no + 1,
                                  RBD_BACKFILL_VISITED_ZERO,
                                  boost::optional<uint8_t>());
    auto* comp = librados::Rados::aio_create_completion();
    int r = md_ctx.aio_operate(visited_oid, comp, &op);
    if (r < 0) {
      ldout(cct, 10) << "note_known_zero_object: aio_operate failed for "
                     << visited_oid << ": " << cpp_strerror(r)
                     << " -- bitmap bit not persisted (non-fatal)" << dendl;
    }
    comp->release();
  }

  bool ImageCtx::is_known_zero_object(uint64_t object_no) {
    Mutex::Locker locker(known_zero_lock);
    return known_zero_objects.count(object_no) != 0;
  }

  void ImageCtx::note_spilled_range(uint64_t off, uint64_t len) {
    if (len == 0) {
      return;
    }
    {
      Mutex::Locker locker(pending_backfill_lock);
      // union_insert, not insert: spills of the same object can overlap and
      // interval_set::insert ceph_aborts on overlap.
      pending_backfill.union_insert(off, len);
    }
    // First spill on this ImageCtx: offload cache completion to the
    // rbd-backfill daemon (read, partial write, and flatten all funnel their
    // spills through here).  Guarded by an atomic so the schedule metadata is
    // written at most once per ImageCtx, regardless of how many objects spill.
    bool expected = false;
    if (backfill_offload_requested.compare_exchange_strong(expected, true)) {
      schedule_self_backfill();
    }
  }

  void ImageCtx::schedule_self_backfill() {
    // Durably set the backfill schedule keys on this image's header so the
    // rbd-backfill daemon adopts it and completes the cache: caches non-zero
    // objects and hole-marks zeros (VISITED_ZERO), never writing zeros.
    // Whole-image backfill is crash-safe (a restarted daemon re-adopts the
    // in_progress image) and cheap on re-runs (per-object stat-skip); if the
    // image is already scheduled/in_progress the daemon dedups, so an
    // unconditional once-per-ImageCtx write is safe.
    //
    // String literals must stay in sync with rbd::backfill::
    // BACKFILL_SCHEDULED_KEY / BACKFILL_STATUS_KEY and the "true"/"scheduled"
    // values in src/tools/rbd_backfill/Types.h -- librbd cannot include
    // tool-tree headers (same constraint as apply_metadata's "backfill_status"
    // literal).  Metadata values are raw bytes (read back via to_str()), so
    // append, do not encode.  Fire-and-forget on md_ctx (the header's pool);
    // ~ImageCtx's md_ctx.aio_flush() drains it before teardown.
    map<string, bufferlist> data;
    data["backfill_scheduled"].append("true");
    data["backfill_status"].append("scheduled");
    librados::ObjectWriteOperation op;
    cls_client::metadata_set(&op, data);
    auto* comp = librados::Rados::aio_create_completion();
    int r = md_ctx.aio_operate(header_oid, comp, &op);
    if (r < 0) {
      ldout(cct, 10) << "schedule_self_backfill: aio_operate failed for "
                     << header_oid << ": " << cpp_strerror(r)
                     << " -- backfill not scheduled (non-fatal)" << dendl;
    } else {
      ldout(cct, 10) << "schedule_self_backfill: marked " << header_oid
                     << " scheduled for backfill (offloading spilled cache warm)"
                     << dendl;
    }
    comp->release();
  }

  Journal<ImageCtx> *ImageCtx::create_journal() {
    return new Journal<ImageCtx>(*this);
  }

  void ImageCtx::set_image_name(const std::string &image_name) {
    // update the name so rename can be invoked repeatedly
    RWLock::RLocker owner_locker(owner_lock);
    RWLock::WLocker snap_locker(snap_lock);
    name = image_name;
    if (old_format) {
      header_oid = util::old_header_name(image_name);
    }
  }

  void ImageCtx::notify_update() {
    state->handle_update_notification();
    ImageWatcher<>::notify_header_update(md_ctx, header_oid);
  }

  void ImageCtx::notify_update(Context *on_finish) {
    state->handle_update_notification();
    image_watcher->notify_header_update(on_finish);
  }

  exclusive_lock::Policy *ImageCtx::get_exclusive_lock_policy() const {
    ceph_assert(owner_lock.is_locked());
    ceph_assert(exclusive_lock_policy != nullptr);
    return exclusive_lock_policy;
  }

  void ImageCtx::set_exclusive_lock_policy(exclusive_lock::Policy *policy) {
    ceph_assert(owner_lock.is_wlocked());
    ceph_assert(policy != nullptr);
    delete exclusive_lock_policy;
    exclusive_lock_policy = policy;
  }

  journal::Policy *ImageCtx::get_journal_policy() const {
    ceph_assert(snap_lock.is_locked());
    ceph_assert(journal_policy != nullptr);
    return journal_policy;
  }

  void ImageCtx::set_journal_policy(journal::Policy *policy) {
    ceph_assert(snap_lock.is_wlocked());
    ceph_assert(policy != nullptr);
    delete journal_policy;
    journal_policy = policy;
  }

  bool ImageCtx::is_writeback_cache_enabled() const {
    return (cache && cache_max_dirty > 0);
  }

  void ImageCtx::get_thread_pool_instance(CephContext *cct,
                                          ThreadPool **thread_pool,
                                          ContextWQ **op_work_queue) {
    auto thread_pool_singleton =
      &cct->lookup_or_create_singleton_object<ThreadPoolSingleton>(
	"librbd::thread_pool", false, cct);
    *thread_pool = thread_pool_singleton;
    *op_work_queue = thread_pool_singleton->op_work_queue;
  }

  void ImageCtx::get_timer_instance(CephContext *cct, SafeTimer **timer,
                                    Mutex **timer_lock) {
    auto safe_timer_singleton =
      &cct->lookup_or_create_singleton_object<SafeTimerSingleton>(
	"librbd::journal::safe_timer", false, cct);
    *timer = safe_timer_singleton;
    *timer_lock = &safe_timer_singleton->lock;
  }
}
