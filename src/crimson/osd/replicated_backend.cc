#include "replicated_backend.h"

#include "messages/MOSDRepOpReply.h"

#include "crimson/common/log.h"
#include "crimson/os/cyan_collection.h"
#include "crimson/os/cyan_object.h"
#include "crimson/os/futurized_store.h"
#include "crimson/osd/shard_services.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_osd);
  }
  static constexpr int TICK_INTERVAL = 1;
}

ReplicatedBackend::ReplicatedBackend(pg_t pgid, pg_shard_t whoami,
                                     ReplicatedBackend::CollectionRef coll,
                                     ceph::osd::ShardServices& shard_services)
  : PGBackend{whoami.shard, coll, &shard_services.get_store()},
    pgid{pgid},
    whoami{whoami},
    shard_services{shard_services}
{}

seastar::future<bufferlist> ReplicatedBackend::_read(const hobject_t& hoid,
                                                     uint64_t off,
                                                     uint64_t len,
                                                     uint32_t flags)
{
  return store->read(coll, ghobject_t{hoid}, off, len, flags);
}

seastar::future<ceph::osd::acked_peers_t>
ReplicatedBackend::_submit_transaction(std::set<pg_shard_t> pg_shards,
                                       const hobject_t& hoid,
                                       ceph::os::Transaction&& txn,
                                       osd_reqid_t req_id,
                                       epoch_t min_epoch, epoch_t map_epoch,
                                       eversion_t ver)
{
  const ceph_tid_t tid = next_txn_id++;
  auto pending_txn = pending_trans.emplace(tid, pending_on_t{}).first;
  bufferlist encoded_txn;
  encode(txn, encoded_txn);

  return seastar::parallel_for_each(pg_shards.begin(), pg_shards.end(),
    [=, encoded_txn=std::move(encoded_txn), txn=std::move(txn)]
    (auto pg_shard) mutable {
      if (pg_shard == whoami) {
        return shard_services.get_store().do_transaction(coll,std::move(txn));
      } else {
        auto m = make_message<MOSDRepOp>(req_id, whoami,
                                         spg_t{pgid, pg_shard.shard}, hoid,
                                         CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK,
                                         map_epoch, min_epoch,
                                         tid, ver);
        m->set_data(encoded_txn);
        // TODO: set more stuff. e.g., pg_states
        return shard_services.send_to_osd(pg_shard.osd, std::move(m), map_epoch);
      }
    }).then([tid, pending_txn, this] {
      auto acked_peers = std::move(pending_txn->second.promise);
      pending_trans.erase(pending_txn);
      return acked_peers.get_future();
    });
}

void ReplicatedBackend::got_rep_op_reply(const MOSDRepOpReply& reply)
{
  auto found = pending_trans.find(reply.get_tid());
  if (found == pending_trans.end()) {
    logger().warn("{}: no matched pending rep op: {}", __func__, reply);
    return;
  }
  unsigned replied = 0;
  const eversion_t zero;
  auto& peers = found->second;
  for (auto& peer : peers.acked_peers) {
    if (peer.last_complete_ondisk != zero) {
      replied++;
    } else if (peer.shard == reply.from) {
      peer.last_complete_ondisk = reply.get_last_complete_ondisk();
      replied++;
    }
  }
  if (replied == peers.acked_peers.size()) {
    peers.promise.set_value(std::move(peers.acked_peers));
  }
}
