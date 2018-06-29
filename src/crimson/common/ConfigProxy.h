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
class ConfigProxy {
  using Config =
    ceph::internal::md_config_impl<ceph::internal::LockPolicy::SINGLE>;
public:
  using local_config_t = seastar::lw_shared_ptr<Config>;
  using foreign_config_t =
    seastar::future<seastar::foreign_ptr<local_config_t>>
  seastar::future<foreign_config_t>> get(config_ref_t old_config);
  seastar::future<foreign_config_t> set_val(const std::string& key,
					    const char* val);
private:
};

} // namespace ceph::common
