// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include "core/sharded.hh"	// foreign_ptr

/// centralized osdmap cache
class OSDMapCache {
  SharedLRU<epoch_t, const OSDMap> map_cache;
  SimpleLRU<epoch_t, bufferlist> map_bl_cache;
  SimpleLRU<epoch_t, bufferlist> map_bl_inc_cache;
  using cached_map_t = seastar::foreign_ptr<boost::local_shared_ptr<OSDMap>>;
public:
  seastar::future<cached_map_t> add_map(boost::local_shared_ptr<OSDMap>);
  seastar::future<cached_map_t> get_map(epoch_t e);
};
