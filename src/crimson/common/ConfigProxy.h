// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 

#include "common/lock_policy.h"
#include "common/config.h"

namespace ceph::common {

class InvalidConfig final : public std::domain_error {
  explicit InvalidConfig(const std::string& what);
};
class ConfigObserver {
  virtual void handle_config_change() = 0;
};

// a Proxy offering access to the single instance of md_config_impl
class ConfigProxy : public seastar::peering_sharded_service<ConfigProxy> {
  static seastar::sharded<ConfigProxy> _the_proxies;
public:
  seastar::future<> local_set_val(const std::string& key,
				  const char* val);
  // needed by seastar:sharded
  future<> stop();
public:
  ConfigProxy& get_local_proxy() {
    return _the_proxies.local();
  }
};

extern 
} // namespace ceph::common
