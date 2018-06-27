// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

class ObjectStore;

/// centralized osdmap cache
class OSDMapCache {
  SharedLRU<epoch_t, const OSDMap> map_cache;
  SimpleLRU<epoch_t, bufferlist> map_bl_cache;
  SimpleLRU<epoch_t, bufferlist> map_bl_inc_cache;
  using cached_map_t = boost::local_shared_ptr<OSDMap>;
  ObjectStore* store;

public:
  void add_map_bl(ObjectStore::Transaction* t, epoch_t e, bufferlist bl);
  void add_inc_bl(ObjectStore::Transaction* t, epoch_t e, bufferlist bl);

  cached_map_t cache_map(cached_map_t map);

  seastar::future<cached_map_t> get_map(epoch_t e);
  seastar::future<bufferlist> get_map_bl(epoch_t e);
  seastar::future<bufferlist> get_inc_bl(epoch_t e);

private:
  void _cache_map_bl(const bufferlist& bl);
  void _cache_inc_bl(const bufferlist& bl, cached_map_t map);
};
