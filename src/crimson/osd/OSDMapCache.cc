#include "os/ObjectStore.h"

#include "OSDMapCache.h"

void
OSDMapCache::add_map_bl(ObjectStore::Transaction* t,
                        epoch_t e, const bufferlist& bl)
{
  ghobject_t fulloid = get_osdmap_pobject_name(e);
  t->write(coll_t::meta(), fulloid, 0, bl.length(), bl);
}

void
OSDMapCache::add_inc_bl(ObjectStore::Transaction* t,
                        epoch_t e, const bufferlist& bl)
{
  ghobject_t oid = get_inc_osdmap_pobject_name(e);
  t->write(coll_t::meta(), oid, 0, bl.length(), bl);
}

seastar::future<OSDMapCache::cached_map_t>
OSDMapCache::get_map(epoch_t epoch)
{
  if (auto osdmap = map_cache.lookup(epoch); osdmap) {
    return osdmap;
  }
  
  if (epoch > 0) {
    return get_map_bl(epoch)
      .then([this](auto& bl) {
        if (bl.length() > 0) {
          auto osdmap = boost::make_local_shared<OSDMap>();
          osdmap->decode(bl);
          return seastar::make_ready_future<cached_map_t>(
              cache_map(osdmap);
        } else {
          return seastar::make_ready_future<cached_map_t>(nullptr);
        }
      });
  } else {
    return seastar::make_ready_future<cached_map_t>(
        cache_map(boost::make_local_shared<OSDMap>()));
  }
}

OSDMapCache::cached_map_t
OSDMapCache::cache_map(cached_map_t map)
{
  const epoch_t e = o->get_epoch();
  // Dedup against an existing map at a nearby epoch
  if (auto for_dedup = map_cache.lower_bound(e); for_dedup) {
    OSDMap::dedup(for_dedup.get(), osdmap);
  }
  bool existed = false;
  auto l = map_cache.add(e, osdmap, &existed);
  if (existed) {
    delete osdmap;
  }
  return l;
}

seastar::future<bufferlist>
OSDMapCache::get_map_bl(epoch_t epoch)
{
  bufferlist bl;
  if (map_bl_cache.lookup(e, &bl)) {
    return seastar::make_ready_future<bufferlist>(bl);
  }
  return store->read(mete_ch,
                     OSD::get_osdmap_pobject_name(e), 0, 0,
                     CEPH_OSD_OP_FLAG_FADVISE_WILLNEED)
    .then([epoch, this](auto& bl) {
      _cache_map_bl(epoch, bl);
    });
}

seastar::future<bufferlist>
OSDMapCache::get_inc_bl(epoch_t epoch)
{
  bufferlist bl;
  if (map_bl_inc_cache.lookup(e, &bl)) {
    return seastar::make_ready_future<bufferlist>(bl);
  }
  return store->read(mete_ch,
                     OSD::get_inc_osdmap_pobject_name(e), 0, 0,
                     CEPH_OSD_OP_FLAG_FADVISE_WILLNEED)
    .then([epoch, this](auto& bl) {
      _cache_inc_bl(epoch, bl);
    });
}
 
void OSDMapCache::_cache_map_bl(epoch_t e, bufferlist& bl)
{
  // cache a contiguous buffer
  if (bl.get_num_buffers() > 1) {
    bl.rebuild();
  }
  // TODO: avoid using global mempool stats
  // bl.try_assign_to_mempool(mempool::mempool_osd_mapbl);
  map_bl_cache.add(e, bl);
}

void OSDMapCache::_cache_inc_bl(epoch_t e, bufferlist& bl)
{
  // cache a contiguous buffer
  if (bl.get_num_buffers() > 1) {
    bl.rebuild();
  }
  // TODO: avoid using global mempool stats
  // bl.try_assign_to_mempool(mempool::mempool_osd_mapbl);
  map_bl_inc_cache.add(e, bl);
}
