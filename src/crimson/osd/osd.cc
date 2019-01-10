#include "osd.h"

#include "messages/MOSDBoot.h"
#include "messages/MOSDMap.h"
#include "crimson/net/Connection.h"
#include "crimson/net/Messenger.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_osd);
  }

  template<typename Message, typename... Args>
  Ref<Message> make_message(Args&&... args)
  {
    return {new Message{std::forward<Args>(args)...}, false};
  }
}

OSD::OSD(int id,
         ceph::net::Messenger& cluster_msgr,
         ceph::net::Messenger& client_msgr)
  : whoami{id},
    cluster_msgr{cluster_msgr},
    client_msgr{client_msgr},
    monc{client_msgr}
{
  osdmaps[0] = seastar::make_lw_shared<OSDMap>();
  dispatchers.push_front(this);
  dispatchers.push_front(&monc);
}

seastar::future<> OSD::mkfs(uuid_d cluster_fsid, int whoami)
{
  CyanStore store{ceph::common::local_conf().get_val<std::string>("osd_data")};
  uuid_d osd_fsid;
  osd_fsid.generate_random();
  store.write_meta("fsid", osd_fsid.to_string());
  store.write_meta("ceph_fsid", cluster_fsid.to_string());
  store.write_meta("whoami", std::to_string(whoami));
  return seastar::now();
}

seastar::future<> OSD::start()
{
  logger().info("start");
  auto& conf = ceph::common::local_conf();
#if 1
  store = std::make_unique<CyanStore>(conf.get_val<std::string>("osd_data"));
#else
  store.set(ObjectStore::create(nullptr,
                                conf.get_val<std::string>("osd_objectstore"),
                                conf.get_val<std::string>("osd_data"),
                                conf.get_val<std::string>("osd_journal"),
                                conf.get_val<uint64_t>("osd_os_flags")));
#endif
  return read_superblock().then([this] {
    osdmap = get_map(superblock.current_epoch);
    return client_msgr.start(&dispatchers);
  }).then([this] {
    return monc.start();
  }).then([this] {
    return update_crush_device_class();
  }).then([this] {
    return update_crush_location();
  }).then([this] {
    monc.sub_want("osd_pg_creates", last_pg_create_epoch, 0);
    monc.sub_want("mgrmap", 0, 0);
    monc.sub_want("osdmap", 0, 0);
    return monc.renew_subs();
  }).then([this] {
    return start_boot();
  });
}

seastar::future<> OSD::update_crush_device_class()
{
  // TODO
  return seastar::now();
}

seastar::future<> OSD::update_crush_location()
{
  // TODO
  return seastar::now();
}

seastar::future<> OSD::start_boot()
{
  state.set_preboot();
  return monc.get_version("osdmap").then([this](version_t newest, version_t oldest) {
    return _preboot(newest, oldest);
  });
}

seastar::future<> OSD::_preboot(version_t newest, version_t oldest)
{
  if (osdmap->get_epoch() == 0) {
    logger().warn("waiting for initial osdmap");
  } else if (osdmap->is_destroyed(whoami)) {
    logger().warn("osdmap says I am destroyed");
    // provide a small margin so we don't livelock seeing if we
    // un-destroyed ourselves.
    if (osdmap->get_epoch() > newest - 1) {
      throw std::runtime_error("i am destroyed");
    }
  } else if (osdmap->test_flag(CEPH_OSDMAP_NOUP) || osdmap->is_noup(whoami)) {
    logger().warn("osdmap NOUP flag is set, waiting for it to clear");
  } else if (!osdmap->test_flag(CEPH_OSDMAP_SORTBITWISE)) {
    logger().error("osdmap SORTBITWISE OSDMap flag is NOT set; please set it");
  } else if (osdmap->require_osd_release < CEPH_RELEASE_LUMINOUS) {
    logger().error("osdmap require_osd_release < luminous; please upgrade to luminous");
  } else if (false) {
    // TODO: update mon if current fullness state is different from osdmap
  } else if (version_t n = ceph::common::local_conf()->osd_map_message_max;
             osdmap->get_epoch() >= oldest - 1 &&
             osdmap->get_epoch() + n > newest) {
    return _send_boot();
  }
  // get all the latest maps
  if (osdmap->get_epoch() + 1 >= oldest) {
    return osdmap_subscribe(osdmap->get_epoch() + 1, false);
  } else {
    return osdmap_subscribe(oldest - 1, true);
  }
}

seastar::future<> OSD::_send_boot()
{
  state.set_booting();

  entity_addrvec_t hb_back_addrs;
  entity_addrvec_t hb_front_addrs;
  entity_addrvec_t cluster_addrs;

  auto m = make_message<MOSDBoot>(superblock,
                                  osdmap->get_epoch(),
                                  osdmap->get_epoch(),
                                  hb_back_addrs,
                                  hb_front_addrs,
                                  cluster_addrs,
                                  CEPH_FEATURES_ALL);
  return monc.send_message(m);
}

seastar::future<> OSD::stop()
{
  // see also OSD::shutdown()
  return gate.close().then([this] {
    state.set_stopping();
    return monc.stop();
  });
}

seastar::future<> OSD::ms_dispatch(ceph::net::ConnectionRef conn, MessageRef m)
{
  logger().info("ms_dispatch {}", *m);
  switch (m->get_type()) {
  case CEPH_MSG_OSD_MAP:
    return handle_osd_map(conn, boost::static_pointer_cast<MOSDMap>(m));
  default:
    return seastar::now();
  }
}

seastar::future<> OSD::ms_handle_connect(ceph::net::ConnectionRef conn)
{
  if (conn->get_peer_type() != CEPH_ENTITY_TYPE_MON) {
    return seastar::now();
  } else {
    return seastar::now();
  }
}

seastar::future<> OSD::ms_handle_reset(ceph::net::ConnectionRef conn)
{
  // TODO: cleanup the session attached to this connection
  logger().warn("ms_handle_reset");
  return seastar::now();
}

seastar::future<> OSD::ms_handle_remote_reset(ceph::net::ConnectionRef conn)
{
  logger().warn("ms_handle_remote_reset");
  return seastar::now();  
}

seastar::lw_shared_ptr<OSDMap> OSD::get_map(epoch_t e)
{
  // TODO: use LRU cache for managing osdmap, fallback to disk if we have to
  return osdmaps[e];
}

void OSD::store_maps(epoch_t start, Ref<MOSDMap> m)
{
  for (epoch_t e = start; e <= m->get_last(); e++) {
    seastar::lw_shared_ptr<OSDMap> o;
    if (auto p = m->maps.find(e); p != m->maps.end()) {
      o = seastar::make_lw_shared<OSDMap>();
      o->decode(p->second);
    } else if (auto p = m->incremental_maps.find(e);
               p != m->incremental_maps.end()) {
      o = get_map(e - 1);
      OSDMap::Incremental inc;
      auto i = p->second.cbegin();
      inc.decode(i);
      o->apply_incremental(inc);
    } else {
      logger().error("MOSDMap lied about what maps it had?");
    }
    osdmaps[e] = std::move(o);
  }
}

seastar::future<> OSD::osdmap_subscribe(version_t epoch, bool force_request)
{
  if (monc.sub_want_increment("osdmap", epoch, CEPH_SUBSCRIBE_ONETIME) ||
      force_request) {
    return monc.renew_subs();
  } else {
    return seastar::now();
  }
}

seastar::future<> OSD::write_superblock()
{
  // TODO
  return seastar::now();
}

seastar::future<> OSD::read_superblock()
{
#if 1
  // just-enough superblock so mon can ack my MOSDBoot
  // might want to have a PurpleStore which is able to read the meta data for us.
  string ceph_fsid = store->read_meta("ceph_fsid");
  superblock.cluster_fsid.parse(ceph_fsid.c_str());
  string osd_fsid = store->read_meta("fsid");
  superblock.osd_fsid.parse(osd_fsid.c_str());

#else
  bufferlist bl;
  if (int r = store->read(meta_ch, OSD_SUPERBLOCK_GOBJECT, 0, 0, bl); r < 0) {
    throw std::runtime_error("unable to read osd superblock");
  }
  auto p = bl.cbegin();
  decode(superblock, p);
#endif
  return seastar::now();
}

seastar::future<> OSD::handle_osd_map(ceph::net::ConnectionRef conn,
                                      Ref<MOSDMap> m)
{
  logger().info("handle_osd_map {}", *m);
  if (m->fsid != superblock.cluster_fsid) {
    logger().warn("fsid mismatched");
    return seastar::now();
  }
  if (state.is_initializing()) {
    logger().warn("i am still initializing");
    return seastar::now();
  }

  const auto first = m->get_first();
  const auto last = m->get_last();

  // make sure there is something new, here, before we bother flushing
  // the queues and such
  if (last <= superblock.newest_map) {
    return seastar::now();
  }
  // missing some?
  bool skip_maps = false;
  epoch_t start = superblock.newest_map + 1;
  if (first > start) {
    logger().info("handle_osd_map message skips epochs {}..{}",
                  start, first - 1);
    if (m->oldest_map <= start) {
      return osdmap_subscribe(start, false);
    }
    // always try to get the full range of maps--as many as we can.  this
    //  1- is good to have
    //  2- is at present the only way to ensure that we get a *full* map as
    //     the first map!
    if (m->oldest_map < first) {
      return osdmap_subscribe(m->oldest_map - 1, true);
    }
    skip_maps = true;
    start = first;
  }
  // TODO: store new maps: queue for disk and put in the osdmap cache
  store_maps(start, m);

  // even if this map isn't from a mon, we may have satisfied our subscription
  monc.sub_got("osdmap", last);
  if (!superblock.oldest_map || skip_maps) {
    superblock.oldest_map = first;
  }
  superblock.newest_map = last;
  superblock.current_epoch = last;

  // note in the superblock that we were clean thru the prior epoch
  if (boot_epoch && boot_epoch >= superblock.mounted) {
    superblock.mounted = boot_epoch;
    superblock.clean_thru = last;
  }
  // TODO: write to superblock and commit the transaction
  return committed_osd_maps(start, last, m);
}

seastar::future<> OSD::committed_osd_maps(version_t first,
                                          version_t last,
                                          Ref<MOSDMap> m)
{
  logger().info("osd.{}: committed_osd_maps({}, {})", whoami, first, last);
  // advance through the new maps
  for (epoch_t cur = first; cur <= last; cur++) {
    osdmap = get_map(cur);
    if (up_epoch != 0 &&
        osdmap->is_up(whoami) &&
        osdmap->get_addrs(whoami) == client_msgr.get_myaddrs()) {
      up_epoch = osdmap->get_epoch();
      if (!boot_epoch) {
        boot_epoch = osdmap->get_epoch();
      }
    }
  }

  if (osdmap->is_up(whoami) &&
      osdmap->get_addrs(whoami) == client_msgr.get_myaddrs() &&
      bind_epoch < osdmap->get_up_from(whoami)) {
    if (state.is_booting()) {
      state.set_active();
    }
  }

  if (state.is_active()) {
    logger().info("osd.{}: now active", whoami);
    if (!osdmap->exists(whoami)) {
      return shutdown();
    }
    if (should_restart()) {
      return restart();
    } else {
      return seastar::now();
    }
  } else if (state.is_preboot()) {
    logger().info("osd.{}: now preboot", whoami);

    if (m->get_source().is_mon()) {
      logger().info("osd.{}: _preboot", whoami);
      return _preboot(m->oldest_map, m->newest_map);
    } else {
      logger().info("osd.{}: start_boot", whoami);
      return start_boot();
    }
  } else {
    logger().info("osd.{}: now ???", whoami);
    // XXX
    return seastar::now();
  }
}

bool OSD::should_restart() const
{
  if (!osdmap->is_up(whoami)) {
    logger().info("map e {} marked osd.{} down",
                  osdmap->get_epoch(), whoami);
    return true;
  } else if (osdmap->get_addrs(whoami) != client_msgr.get_myaddrs()) {
    logger().error("map e {} had wrong client addr ({} != my {})",
                   osdmap->get_epoch(),
                   osdmap->get_addrs(whoami),
                   client_msgr.get_myaddrs());
    return true;
  } else if (osdmap->get_cluster_addrs(whoami) != cluster_msgr.get_myaddrs()) {
    logger().error("map e {} had wrong cluster addr ({} != my {})",
                   osdmap->get_epoch(),
                   osdmap->get_cluster_addrs(whoami),
                   cluster_msgr.get_myaddrs());
    return true;
  } else {
    return false;
  }
}

seastar::future<> OSD::restart()
{
  up_epoch = 0;
  bind_epoch = osdmap->get_epoch();
  // TODO: promote to shutdown if being marked down for multiple times
  // rebind messengers
  return start_boot();
}

seastar::future<> OSD::shutdown()
{
  // TODO
  superblock.mounted = boot_epoch;
  superblock.clean_thru = osdmap->get_epoch();
  return seastar::now();
}
