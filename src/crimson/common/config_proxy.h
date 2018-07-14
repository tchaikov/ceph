// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include "common/config_proxy_impl.h"

namespace ceph::common {

class ConfigProxy final :
  public ceph::internal::config_proxy_impl<ConfigProxy,
					   ceph::internal::LockPolicy::SINGLE>,
  public seastar::peering_sharded_service<ConfigProxy>
{
  using LocalConfigValues = seastar::lw_shared_ptr<ConfigValues>;
  seastar::foreign_ptr<LocalConfigValues> values;

  using Config = ceph::internal::md_config_impl<lock_policy>;
  Config* remote_config;
  std::unique_ptr<Config> local_config;

  // for my CRTP parent
  bool is_local() const {
    return values.get_owner_shard() == seastar::engine().cpu_id();
  }
  friend class config_proxy_impl<Config,
				 ceph::internal::LockPolicy::SINGLE>;
  const ConfigValues& get_values() const {
    return *values;
  }
  ConfigValues& get_values() {
    return *values;
  }
  const Config& get_config() const {
    return is_local() ? *local_config : *remote_config;
  }
  Config& get_config() {
    return is_local() ? *local_config : *remote_config;
  }
  // apply changes to all shards
  // @param func a functor which accepts @c "ConfigValues&", and returns
  //             a boolean indicating if the values is changed or not
  template<typename Func>
  seastar::future<> apply_change(Func&& func) {
    return container().invoke_on(values.get_owner_shard(),
				 [func=std::move(func)] (auto& owner) {
      auto new_values = seastar::make_lw_shared(*owner.values);
      if (func(*new_values)) {
	auto foreign_values = seastar::make_foreign(new_values);
	return owner.container().invoke_on_all([foreign_values](auto& proxy) {
	  return foreign_values.copy([&proxy](auto foreign_values) {
	    proxy.values = foreign_values;
          });
        });
      }
    });
  }
public:
  ConfigProxy();
  seastar::future<> run();

  seastar::future<> rm_val(const std::string& key) {
    return apply_change([key, this](ConfigValues& values) {
      return local_config->rm_val(values, key) >= 0;
    });
  }
  seastar::future<> set_val(const std::string& key,
			    const std::string& val) {
    return apply_change([key, val, this](ConfigValues& values) {
      std::stringstream err;
      auto ret = local_config->set_val(values, key, val, &err);
      if (ret < 0) {
	throw std::invalid_argument(err.str());
      }
      if (ret > 0) {
        local_config->apply_changes(values, *this, nullptr);
	return true;
      } else {
	return false;
      }
    });
  }
  seastar::future<> set_mon_vals(const std::map<std::string,std::string>& kv) {
    return apply_changes([kv, this](ConfigValues& values) {
      aut ret = local_config->set_mon_vals(nullptr, values, *this, kv,
					   nullptr);
      return ret > 0;
    });
  }
};

}
