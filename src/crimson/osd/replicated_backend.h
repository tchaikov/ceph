// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <seastar/core/future.hh>
#include "include/buffer_fwd.h"
#include "osd/osd_types.h"

#include "acked_peers.h"
#include "pg_backend.h"

namespace ceph::osd {
  class ShardServices;
}

class ReplicatedBackend : public PGBackend
{
public:
  ReplicatedBackend(pg_t pgid, pg_shard_t whoami,
		    CollectionRef coll,
		    ceph::osd::ShardServices& shard_services);
  void got_rep_op_reply(const MOSDRepOpReply& reply) final;
private:
  seastar::future<ceph::bufferlist> _read(const hobject_t& hoid,
					  uint64_t off,
					  uint64_t len,
					  uint32_t flags) override;
  seastar::future<ceph::osd::acked_peers_t>
  _submit_transaction(std::set<pg_shard_t> pg_shards,
		      const hobject_t& hoid,
		      ceph::os::Transaction&& txn,
		      osd_reqid_t req_id,
		      epoch_t min_epoch, epoch_t max_epoch,
		      eversion_t ver) final;
  const pg_t pgid;
  const pg_shard_t whoami;
  ceph::osd::ShardServices& shard_services;
  ceph_tid_t next_txn_id = 0;
  struct pending_on_t {
    ceph::osd::acked_peers_t acked_peers;
    seastar::promise<ceph::osd::acked_peers_t> promise;
  };
  using pending_transactions_t = std::map<ceph_tid_t, pending_on_t>;
  pending_transactions_t pending_trans;
};
