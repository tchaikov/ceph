// -*- mode: c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

namespace crimson::os::seastore {

class OMap
{
public:
  OMap();
  using omap_values_t = std::map<std::string, bufferlist, std::less<>>;
  using omap_keys_t = std::set<std::string>;
  seastar::future<omap_values_t> get_values_by_keys(const ghobject_t& oid,
						    omap_keys_t&& keys);
  seastar::future<omap_values_t> get_values(const ghobject_t& oid,
					    omap_keys_t&& keys);
  seastar::future<omap_values_t> get_values(const ghobject_t& oid,
					    string&& start,
					    string&& prefix,
					    uint64_t max_return);
  seastar::future<> set(omap)
  seastar::future<> remove_by_keys(omap_keys_t&& keys);
};

} // crimson::os::seastore
