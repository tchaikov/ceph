#pragma once

#include <map>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_ptr.hh>

#include "crimson/mon/MonClient.h"
#include "crimson/net/Dispatcher.h"
#include "crimson/os/cyan_store.h"
#include "crimson/osd/chained_dispatcher.h"
#include "crimson/osd/state.h"

#include "osd/OSDMap.h"

class MOSDMap;
class OSDMap;

class OSD : public ceph::net::Dispatcher {
  seastar::gate gate;

  const int whoami;
  ceph::net::Messenger& cluster_msgr;
  ceph::net::Messenger& client_msgr;
  ChainedDispatchers dispatchers;
  ceph::mon::Client monc;

  // TODO: use LRU cache
  std::map<epoch_t, seastar::lw_shared_ptr<OSDMap>> osdmaps;
  seastar::lw_shared_ptr<OSDMap> osdmap;
  // TODO: use a wrapper for ObjectStore
  std::unique_ptr<CyanStore> store;

  OSDState state;

  /// _first_ epoch we were marked up (after this process started)
  epoch_t boot_epoch = 0;
  /// _most_recent_ epoch we were marked up
  epoch_t up_epoch = 0;
  //< epoch we last did a bind to new ip:ports
  epoch_t bind_epoch = 0;
  //< since when there is no more pending pg creates from mon
  epoch_t last_pg_create_epoch = 0;

  OSDSuperblock superblock;

  // Dispatcher methods
  seastar::future<> ms_dispatch(ceph::net::ConnectionRef conn, MessageRef m) override;
  seastar::future<> ms_handle_connect(ceph::net::ConnectionRef conn) override;
  seastar::future<> ms_handle_reset(ceph::net::ConnectionRef conn) override;
  seastar::future<> ms_handle_remote_reset(ceph::net::ConnectionRef conn) override;

public:
  OSD(int id,
      ceph::net::Messenger& cluster_msgr,
      ceph::net::Messenger& client_msgr);
  ~OSD() = default;

  static seastar::future<> mkfs(uuid_d fsid, int whoami);

  seastar::future<> start();
  seastar::future<> stop();

private:
  seastar::future<> update_crush_device_class();
  seastar::future<> update_crush_location();
  seastar::future<> start_boot();
  seastar::future<> _preboot(version_t newest_osdmap, version_t oldest_osdmap);
  seastar::future<> _send_boot();

  seastar::lw_shared_ptr<OSDMap> get_map(epoch_t e);
  // TODO: should batch the write op along with superdisk modification as a
  //       transaction
  void store_maps(epoch_t start, Ref<MOSDMap> m);
  seastar::future<> osdmap_subscribe(version_t epoch, bool force_request);
  seastar::future<> write_superblock();
  seastar::future<> read_superblock();

  seastar::future<> handle_osd_map(ceph::net::ConnectionRef conn,
                                   Ref<MOSDMap> m);
  seastar::future<> committed_osd_maps(version_t first,
                                       version_t last,
                                       Ref<MOSDMap> m);
  bool should_restart() const;
  seastar::future<> restart();
  seastar::future<> shutdown();
};
