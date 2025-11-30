// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_BACKFILL_THROTTLER_H
#define CEPH_RBD_BACKFILL_THROTTLER_H

#include "include/int_types.h"
#include "common/Mutex.h"
#include "common/Cond.h"
#include <list>
#include <map>
#include <set>

class CephContext;
class Context;
class ContextWQ;

namespace rbd {
namespace backfill {

class ObjectBackfillRequest;

class BackfillThrottler {
public:
  BackfillThrottler(CephContext *cct, ContextWQ *work_queue);
  ~BackfillThrottler();

  BackfillThrottler(const BackfillThrottler&) = delete;
  BackfillThrottler& operator=(const BackfillThrottler&) = delete;

  void start_op(uint64_t obj_no, Context *on_start);
  void finish_op(uint64_t obj_no);

  void set_max_concurrent(uint32_t max_concurrent);
  uint32_t get_max_concurrent() const;

  void get_status(uint32_t *inflight, uint32_t *queued) const;

  // Wait for all inflight operations to complete
  void wait_for_ops();

private:
  void start_next_op();

  CephContext *m_cct;
  ContextWQ *m_work_queue;

  mutable Mutex m_lock;
  Cond m_cond;  // Condition variable for waiting on operations
  uint32_t m_max_concurrent;

  std::set<uint64_t> m_inflight_ops;

  struct QueuedOp {
    uint64_t obj_no;
    Context *on_start;
  };
  std::list<QueuedOp> m_queued_ops;
};

} // namespace backfill
} // namespace rbd

#endif // CEPH_RBD_BACKFILL_THROTTLER_H
