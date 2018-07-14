// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include <type_traits>

#include "common/config.h"
#include "common/config_fwd.h"
#include "common/lock_policy.h"

namespace ceph::internal {
// the shared part of ConfigProxy and its seastar counterpart
template<class ConfigType, LockPolicy lp>
class config_proxy_impl {
protected:
  static constexpr auto lock_policy = lp;

private:
  /**
   * The current values of all settings described by the schema
   */
  const ConfigValues& get_values() const {
    return static_cast<const ConfigType*>(this)->get_values();
  }
  ConfigValues& get_values() {
    return static_cast<ConfigType*>(this)->get_values();
  }
  const md_config_impl<lock_policy>& get_config() const {
    return static_cast<const ConfigType*>(this)->get_config();
  }
  md_config_impl<lock_policy>& get_config() {
    return static_cast<ConfigType*>(this)->get_config();
  }

public:
  const ConfigValues* operator->() const noexcept {
    return &get_values();
  }
  ConfigValues* operator->() noexcept {
    return &get_values();
  }
  int get_val(const std::string& key, char** buf, int len) const {
    return get_config().get_val(get_values(), key, buf, len);
  }
  int get_val(const std::string &key, std::string *val) const {
    return get_config().get_val(get_values(), key, val);
  }
  template<typename T>
  const T get_val(const std::string& key) const {
    return get_config().template get_val<T>(get_values(), key);
  }
  template<typename T, typename Callback, typename...Args>
  auto with_val(const string& key, Callback&& cb, Args&&... args) const {
    return get_config().template with_val<T>(get_values(), key,
					      std::forward<Callback>(cb),
					      std::forward<Args>(args)...);
  }
  void show_config(std::ostream& out) const {
    get_config().show_config(get_values(), out);
  }
  void show_config(Formatter *f) const {
    get_config().show_config(get_values(), f);
  }
  void config_options(Formatter *f) const {
    get_config().config_options(f);
  }
  const Option* get_schema(const std::string& key) const {
    auto found = get_config().schema.find(key);
    if (found == get_config().schema.end()) {
      return nullptr;
    } else {
      return &found->second;
    }
  }
  const Option *find_option(const string& name) const {
    return get_config().find_option(name);
  }
  void diff(Formatter *f, const std::string& name=string{}) const {
    return get_config().diff(get_values(), f, name);
  }
  void get_my_sections(std::vector <std::string> &sections) const {
    get_config().get_my_sections(get_values(), sections);
  }
  int get_all_sections(std::vector<std::string>& sections) const {
    return get_config().get_all_sections(sections);
  }
  int get_val_from_conf_file(const std::vector<std::string>& sections,
			     const std::string& key, std::string& out,
			     bool emeta) const {
    return get_config().get_val_from_conf_file(get_values(),
					       sections, key, out, emeta);
  }
  unsigned get_osd_pool_default_min_size() const {
    return get_config().get_osd_pool_default_min_size(get_values());
  }
  void early_expand_meta(std::string &val,
			 std::ostream *oss) const {
    return get_config().early_expand_meta(get_values(), val, oss);
  }
  size_t num_parse_errors() const {
    return get_config().parse_errors.size();
  }
  void complain_about_parse_errors(CephContext *cct) {
    return get_config().complain_about_parse_errors(cct);
  }
  void do_argv_commands() const {
    get_config().do_argv_commands(get_values());
  }
  void get_config_bl(uint64_t have_version,
		     bufferlist *bl,
		     uint64_t *got_version) {
    get_config().get_config_bl(get_values(), have_version, bl, got_version);
  }
  void get_defaults_bl(bufferlist *bl) {
    get_config().get_defaults_bl(get_values(), bl);
  }
};
}
