// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <set>
#include <seastar/core/future.hh>

#include "osd/osd_types.h"
#include "crimson/osd/osd_operation.h"
#include "crimson/osd/acked_peers.h"

struct ObjectState;

namespace ceph::os {
  struct Collection;
}

namespace ceph::osd {

class ShardServices;

class RepOps {
public:
  RepOps(pg_t pgid, pg_shard_t pg_whoami, ShardServices& shard_service);
  seastar::future<acked_peers_t>
  submit_transaction(std::set<pg_shard_t> pg_shards,
		     boost::intrusive_ptr<ceph::os::Collection> coll,
		     const hobject_t& hoid,
		     ceph::os::Transaction&& txn,
		     osd_reqid_t req_id,
		     epoch_t min_epoch,
		     epoch_t map_epoch,
		     eversion_t ver);
  void got_reply(const MOSDRepOpReply& reply);

private:
  struct RepOpBlocker : public Blocker {
    bool acked_by_me = false;
    acked_peers_t acked_by_peers;
    seastar::promise<acked_peers_t> promise;
    void dump_detail(Formatter *f) const final;
    const char* get_type_name() const final;
  };
  using pending_transactions_t = std::map<ceph_tid_t, RepOpBlocker>;
  pending_transactions_t pending_trans;

  const pg_t pgid;
  const pg_shard_t pg_whoami;
  ceph_tid_t txn_id = 0;
  ShardServices& shard_services;
};

}
