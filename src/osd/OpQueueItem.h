// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#pragma once

#include <ostream>

#include "include/types.h"
#include "include/utime.h"
#include "osd/OpRequest.h"
#include "osd/PG.h"


class OSD;


struct PGScrub {
  epoch_t epoch_queued;
  explicit PGScrub(epoch_t e) : epoch_queued(e) {}
  ostream &operator<<(ostream &rhs) {
    return rhs << "PGScrub";
  }
};

struct PGSnapTrim {
  epoch_t epoch_queued;
  explicit PGSnapTrim(epoch_t e) : epoch_queued(e) {}
  ostream &operator<<(ostream &rhs) {
    return rhs << "PGSnapTrim";
  }
};

struct PGRecovery {
  epoch_t epoch_queued;
  uint64_t reserved_pushes;
  PGRecovery(epoch_t e, uint64_t reserved_pushes)
    : epoch_queued(e), reserved_pushes(reserved_pushes) {}
  ostream &operator<<(ostream &rhs) {
    return rhs << "PGRecovery(epoch=" << epoch_queued
	       << ", reserved_pushes: " << reserved_pushes << ")";
  }
};

class OpQueueItem {
public:
  class OrderLocker {
  public:
    using Ref = unique_ptr<OrderLocker>;
    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual ~OrderLocker() {}
  };
  // Abstraction for operations queueable in the op queue
  class OpQueueable {
  public:
    enum class op_type_t {
      client_op,
      osd_subop,
      bg_snaptrim,
      bg_recovery,
      bg_scrub
    };
    using Ref = std::unique_ptr<OpQueueable>;

    /// Items with the same queue token will end up in the same shard
    virtual uint32_t get_queue_token() const = 0;

    /// Items will be dequeued and locked atomically w.r.t. other items with the
    /// same ordering token
    virtual size_t get_ordering_token() const = 0;
    virtual OrderLocker::Ref get_order_locker(PGRef pg) = 0;
    virtual op_type_t get_op_type() const = 0;
    virtual boost::optional<OpRequestRef> maybe_get_op() const {
      return boost::none;
    }

    virtual uint64_t get_reserved_pushes() const {
      return 0;
    }

    virtual ostream &print(ostream &rhs) const = 0;

    virtual void run(OSD *osd, PGRef& pg, ThreadPool::TPHandle &handle) = 0;
    virtual ~OpQueueable() {}
    friend ostream& operator<<(ostream& out, const OpQueueable& q) {
      return q.print(out);
    }
  };

private:
  OpQueueable::Ref qitem;
  int cost;
  unsigned priority;
  utime_t start_time;
  entity_inst_t owner;
  epoch_t map_epoch;    ///< an epoch we expect the PG to exist in

public:
  OpQueueItem(
    OpQueueable::Ref &&item,
    int cost,
    unsigned priority,
    utime_t start_time,
    const entity_inst_t &owner,
    epoch_t e)
    : qitem(std::move(item)),
      cost(cost),
      priority(priority),
      start_time(start_time),
      owner(owner),
      map_epoch(e)
  {}
  OpQueueItem(OpQueueItem &&) = default;
  OpQueueItem(const OpQueueItem &) = delete;
  OpQueueItem &operator=(OpQueueItem &&) = default;
  OpQueueItem &operator=(const OpQueueItem &) = delete;

  OrderLocker::Ref get_order_locker(PGRef pg) {
    return qitem->get_order_locker(pg);
  }
  uint32_t get_queue_token() const {
    return qitem->get_queue_token();
  }
  size_t get_ordering_token() const {
    return qitem->get_ordering_token();
  }
  using op_type_t = OpQueueable::op_type_t;
  OpQueueable::op_type_t get_op_type() const {
    return qitem->get_op_type();
  }
  boost::optional<OpRequestRef> maybe_get_op() const {
    return qitem->maybe_get_op();
  }
  uint64_t get_reserved_pushes() const {
    return qitem->get_reserved_pushes();
  }
  void run(OSD *osd, PGRef& pg, ThreadPool::TPHandle &handle) {
    qitem->run(osd, pg, handle);
  }
  unsigned get_priority() const { return priority; }
  int get_cost() const { return cost; }
  utime_t get_start_time() const { return start_time; }
  entity_inst_t get_owner() const { return owner; }
  epoch_t get_map_epoch() const { return map_epoch; }

  friend ostream& operator<<(ostream& out, const OpQueueItem& item) {
    return out << "OpQueueItem("
	       << item.get_ordering_token() << " " << *item.qitem
	       << " prio " << item.get_priority()
	       << " cost " << item.get_cost()
	       << " e" << item.get_map_epoch() << ")";
  }
};

/// Implements boilerplate for operations queued for the pg lock
class PGOpQueueable : public OpQueueItem::OpQueueable {
  spg_t pgid;
protected:
  const spg_t& get_pgid() const {
    return pgid;
  }
public:
  PGOpQueueable(spg_t pg) : pgid(pg) {}
  uint32_t get_queue_token() const override final {
    return get_pgid().ps();
  }

  size_t get_ordering_token() const override final {
    return std::hash<spg_t>{}(get_pgid());
  }

  OpQueueItem::OrderLocker::Ref get_order_locker(PGRef pg) override final {
    class Locker : public OpQueueItem::OrderLocker {
      PGRef pg;
    public:
      Locker(PGRef pg) : pg(pg) {}
      void lock() override final {
	pg->lock();
      }
      void unlock() override final {
	pg->unlock();
      }
    };
    return OpQueueItem::OrderLocker::Ref(
      new Locker(pg));
  }
};

class PGOpItem : public PGOpQueueable {
  OpRequestRef op;
public:
  PGOpItem(spg_t pg, OpRequestRef op) : PGOpQueueable(pg), op(op) {}
  op_type_t get_op_type() const override final {
    if (op->get_req()->get_header().type == MSG_OSD_SUBOP) {
      return op_type_t::osd_subop;
    } else {
      return op_type_t::client_op;
    }
  }
  ostream &print(ostream &rhs) const override final {
    return rhs << "PGOpItem(op=" << *(op->get_req()) << ")";
  }
  boost::optional<OpRequestRef> maybe_get_op() const override final {
    return op;
  }
  void run(OSD *osd, PGRef& pg, ThreadPool::TPHandle &handle) override final;
};

namespace ceph {
  namespace details {
    template<typename Item> class QueueToken {};
    template<>
    class QueueToken<OpQueueItem> {
    const OpQueueItem& item;
    public:
      QueueToken(const OpQueueItem& item)
	: item(item)
      {}
      uint32_t shard_index(size_t nshards) const {
	return item.get_queue_token() % nshards;
      }
      size_t ordering_token() const {
	return item.get_ordering_token();
      }
    };
    template<>
    class QueueToken<spg_t> {
      const spg_t pgid;
    public:
      QueueToken(const spg_t& pgid)
	: pgid(pgid)
      {}
      uint32_t shard_index(size_t nshards) const {
	return pgid.ps() % nshards;
      }
      size_t ordering_token() const {
	return std::hash<spg_t>{}(pgid);
      }
    };
  } // namaspace details
  template<typename T> uint32_t get_shard_index(const T& item, size_t shards) {
    return details::QueueToken<T>{item}.queue_token() % shards;
  }
  template<typename T> size_t get_ordering_token(const T& item) {
    return details::QueueToken<T>{item}.ordering_token();
  }
}
