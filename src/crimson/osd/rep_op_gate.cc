// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rep_op_gate.h"

#include "messages/MOSDRepOpReply.h"
#include "os/Transaction.h"
#include "osd/osd_internal_types.h"
#include "crimson/os/futurized_store.h"

#include "shard_services.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_osd);
  }
}

namespace ceph::osd {

void RepOps::RepOpBlocker::dump_detail(Formatter *f) const
{
  f->open_object_section("RepOps");
  {
    f->dump_bool("local_acked", acked_by_me);
    f->open_array_section("remote_acked");
    const eversion_t zero;
    for (auto& peer : acked_by_peers) {
      f->open_object_section("peer");
      {
        f->dump_stream("shard") << peer.shard;
        f->dump_bool("acked", peer.last_complete_ondisk != zero);
      }
      f->close_section();
    }
    f->close_section();
  }
  f->close_section();
}

RepOps::RepOps(pg_t pgid, pg_shard_t pg_whoami,
               ShardServices& shard_service)
  : pgid{pgid},
    pg_whoami{pg_whoami},
    shard_services{shard_services}
{}

seastar::future<ceph::osd::acked_peers_t>
RepOps::submit_transaction(std::set<pg_shard_t> pg_shards,
                           ceph::os::FuturizedStore::CollectionRef coll,
                           const hobject_t& hoid,
                           ceph::os::Transaction&& txn,
			   osd_reqid_t req_id,
                           epoch_t min_epoch,
                           epoch_t map_epoch,
                           eversion_t ver)
{
  const auto tid = txn_id++;

  RepOpBlocker& blocker = pending_trans[tid];

  bufferlist encoded_txn;
  encode(txn, encoded_txn);
  return blocker.make_blocking_future(
    seastar::parallel_for_each(pg_shards.begin(), pg_shards.end(),
      [=, encoded_txn=std::move(encoded_txn), txn=std::move(txn), &blocker]
      (pg_shard_t pg_shard) mutable {
        if (pg_shard == pg_whoami) {
          return shard_services.get_store().do_transaction(
            coll,std::move(txn)).then([&blocker] {
            blocker.acked_by_me = true;
            return seastar::now();
          });
        } else {
          auto m = make_message<MOSDRepOp>(req_id, pg_whoami,
                                           spg_t{pgid, pg_shard.shard}, hoid,
                                           CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK,
                                           map_epoch, min_epoch,
                                           tid, ver);
          m->set_data(encoded_txn);
          // TODO: set more stuff. e.g., pg_states
          return shard_services.send_to_osd(pg_shard.osd, std::move(m), map_epoch);
        }
      })
    .then([&blocker] {
      return blocker.promise.get_future();
    }));
}

void RepOps::got_reply(const MOSDRepOpReply& reply)
{
  if (auto found = pending_trans.find(reply.get_tid()); found != pending_trans.end()) {
    unsigned replied = 0;
    eversion_t zero;
    auto& acked_by_peers = found->second.acked_by_peers;
    for (auto& peer_shard : acked_by_peers) {
      if (peer_shard.last_complete_ondisk != zero) {
        replied++;
      } else if (peer_shard.shard == reply.from) {
        peer_shard.last_complete_ondisk = reply.get_last_complete_ondisk();
        replied++;
      }
    }
    if (replied == found->second.acked_by_peers.size()) {
      found->second.promise.set_value(std::move(acked_by_peers));
    }
  } else {
    logger().warn("{}: no matched pending rep op: {}", __func__, reply);
  }
}

}
