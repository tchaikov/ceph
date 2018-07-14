// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include "common/config.h"
#include "common/config_proxy_impl.h"

class ConfigProxy final :
  public ceph::internal::config_proxy_impl<ConfigProxy,
					   ceph::internal::LockPolicy::MUTEX> {
  /**
   * The current values of all settings described by the schema
   */
  ConfigValues values;
  md_config_t config;

  friend class config_proxy_impl<ConfigProxy,
				 ceph::internal::LockPolicy::MUTEX>;
  const ConfigValues& get_values() const {
    return values;
  }
  ConfigValues& get_values() {
    return values;
  }
  const md_config_t& get_config() const {
    return config;
  }
  md_config_t& get_config() {
    return config;
  }

public:
  explicit ConfigProxy(bool is_daemon)
    : config{values, is_daemon}
  {}
  // change `values` in-place
  void finalize_reexpand_meta() {
    config.finalize_reexpand_meta(get_values(), *this);
  }
  void add_observer(md_config_obs_t* obs) {
    config.add_observer(obs);
  }
  void remove_observer(md_config_obs_t* obs) {
    config.remove_observer(obs);
  }
  void set_safe_to_start_threads() {
    config.set_safe_to_start_threads();
  }
  void _clear_safe_to_start_threads() {
    config._clear_safe_to_start_threads();
  }
  void call_all_observers() {
    config.call_all_observers(*this);
  }
  int rm_val(const std::string& key) {
    return config.rm_val(values, key);
  }
  void apply_changes(std::ostream* oss) {
    config.apply_changes(values, *this, oss);
  }
  int set_val(const std::string& key, const std::string& s,
              std::stringstream* err_ss=nullptr) {
    return config.set_val(values, key, s);
  }
  void set_val_default(const std::string& key, const std::string& val) {
    config.set_val_default(values, key, val);
  }
  void set_val_or_die(const std::string& key, const std::string& val) {
    config.set_val_or_die(values, key, val);
  }
  int set_mon_vals(CephContext *cct,
		   const map<std::string,std::string>& kv,
		   md_config_t::config_callback config_cb) {
    return config.set_mon_vals(cct, values, *this, kv, config_cb);
  }
  int injectargs(const std::string &s, std::ostream *oss) {
    return config.injectargs(values, *this, s, oss);
  }
  void parse_env(const char *env_var = "CEPH_ARGS") {
    config.parse_env(values, env_var);
  }
  int parse_argv(std::vector<const char*>& args, int level=CONF_CMDLINE) {
    return config.parse_argv(values, args, level);
  }
  int parse_config_files(const char *conf_files,
			 std::ostream *warnings, int flags) {
    return config.parse_config_files(values, conf_files, warnings, flags);
  }
};
