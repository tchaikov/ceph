// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#include "common/options.h"

namespace ceph::internal {
template<LockPolicy lp> class LockConfigValues;

using values_t = std::map<std::string, map<int32_t,Option::value_t>>;

class ConfigValues {
  using changed_set_t = std::set<std::string>;
  changed_set_t changed;
  values_t values;
  ceph::logging::SubsystemMap subsys;
public:
  /**
   * @return true if changed, false otherwise
   */
  bool set_value(const std::string& name,
		 Option::value_t&& value,
		 int level);
private:
  _refresh();
};

template<>
class LockConfigValues<LockPolicy::MUTEX> {
  ConfigValues values;
public:
  values_t& operator*() const noexcept {
    return values;
  }
  bool set_value(const std::string& name,
		 Option::value_t&& value,
		 int level);
};

template<>
class LockConfigValues<LockPolicy::SINGLE> {
  seastar::lw_shared_ptr<ConfigValues>> pending_values;
  seastar::foreign_ptr<seastar::lw_shared_ptr<ConfigValues>> values;
public:
  values_t& operator*() const noexcept {
    return *(values.get());
  }
  seastar::future<> set_value(const std::string& name,
			      Option::value_t&& value,
			      int level);
};
