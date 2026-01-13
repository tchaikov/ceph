// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/RemoteClusterUtils.h"
#include "common/ceph_context.h"
#include "common/config.h"
#include "common/dout.h"
#include "common/errno.h"
#include "include/encoding.h"
#include <fstream>
#include <sstream>
#include <boost/algorithm/string.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::RemoteClusterUtils: "

namespace librbd {
namespace util {

int parse_mon_hosts_from_config(const std::string& conf_path,
                                std::vector<std::string>& mon_hosts,
                                std::string& cluster_name) {
  int r;

  // Use RADOS API to parse config file instead of manual parsing
  // This handles all the INI file complexities (includes, variable expansion, etc.)
  librados::Rados temp_cluster;

  // Initialize without connecting (just for config parsing)
  r = temp_cluster.init("admin");
  if (r < 0) {
    return r;
  }

  // Read the configuration file
  r = temp_cluster.conf_read_file(conf_path.c_str());
  if (r < 0) {
    temp_cluster.shutdown();
    return (r == -ENOENT) ? -ENOENT : r;
  }

  // Get mon_host configuration value
  std::string mon_host_str;
  r = temp_cluster.conf_get("mon_host", mon_host_str);
  if (r < 0) {
    temp_cluster.shutdown();
    return -EINVAL;
  }

  // Extract cluster name from config file path
  // Logic from src/common/config.cc lines 400-409:
  // Use the prefix of the basename (before .conf) as cluster name
  auto start = conf_path.rfind('/') + 1;
  auto end = conf_path.find(".conf", start);
  if (end == std::string::npos) {
    // Configuration file doesn't follow $cluster.conf convention
    cluster_name = "ceph";  // default
  } else {
    cluster_name = conf_path.substr(start, end - start);
  }

  // Clean up temporary cluster handle
  temp_cluster.shutdown();

  // Parse comma or space-separated monitor addresses
  if (mon_host_str.empty()) {
    return -EINVAL;
  }

  std::vector<std::string> addrs;
  boost::split(addrs, mon_host_str, boost::is_any_of(", "), boost::token_compress_on);

  for (const auto& addr : addrs) {
    std::string trimmed = addr;
    boost::trim(trimmed);
    if (!trimmed.empty()) {
      mon_hosts.push_back(trimmed);
    }
  }

  if (mon_hosts.empty()) {
    return -EINVAL;
  }

  return 0;
}

int read_and_encode_keyring(const std::string& keyring_path,
                            const std::string& client_name,
                            std::string& encoded_keyring) {
  std::ifstream keyring_file(keyring_path);
  if (!keyring_file.is_open()) {
    return -ENOENT;
  }

  const size_t MAX_LINE_LENGTH = 16384;  // 16KB max line length
  std::string line;
  bool found_client = false;
  std::string key_value;

  while (std::getline(keyring_file, line)) {
    // Skip overly long lines to prevent memory exhaustion
    if (line.size() > MAX_LINE_LENGTH) {
      continue;
    }

    boost::trim(line);

    // Skip empty lines and comments
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }

    // Check for client section
    if (line[0] == '[') {
      size_t close_bracket = line.find(']');
      if (close_bracket == std::string::npos || close_bracket == 0) {
        // Malformed section header, skip
        continue;
      }
      std::string section = line.substr(1, close_bracket - 1);
      boost::trim(section);
      found_client = (section == client_name);
      continue;
    }

    if (found_client) {
      // Look for key = value
      size_t eq_pos = line.find('=');
      if (eq_pos != std::string::npos) {
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        boost::trim(key);
        boost::trim(value);

        if (key == "key") {
          key_value = value;
          break;
        }
      }
    }
  }

  if (key_value.empty()) {
    return -EINVAL;
  }

  // Key is already base64-encoded in keyring file, no need to encode again
  encoded_keyring = key_value;

  return 0;
}

int connect_to_remote_cluster(CephContext* cct,
                              const std::string& cluster_name,
                              const std::vector<std::string>& mon_hosts,
                              const std::string& keyring,
                              const std::string& client_name,
                              librados::Rados& cluster) {
  int r;

  // Initialize cluster handle
  r = cluster.init2(client_name.c_str(), cluster_name.c_str(), 0);
  if (r < 0) {
    return r;
  }

  // Set monitor addresses
  std::string mon_host_str;
  for (size_t i = 0; i < mon_hosts.size(); ++i) {
    if (i > 0) {
      mon_host_str += ",";
    }
    mon_host_str += mon_hosts[i];
  }

  r = cluster.conf_set("mon_host", mon_host_str.c_str());
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  // Set the key directly via conf_set (no file needed - more secure!)
  // Key is already base64-encoded from keyring file
  r = cluster.conf_set("key", keyring.c_str());
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  // Disable loading keyring from files (we set the key directly)
  r = cluster.conf_set("keyring", "");
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  // Inherit timeout settings from current cluster configuration
  // This ensures consistency and respects user's tuning for their environment
  auto& conf = cct->_conf;

  std::ostringstream oss;
  oss << conf->rados_osd_op_timeout;
  r = cluster.conf_set("rados_osd_op_timeout", oss.str().c_str());
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  oss.str("");
  oss << conf->client_mount_timeout;
  r = cluster.conf_set("client_mount_timeout", oss.str().c_str());
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  oss.str("");
  oss << conf->rados_mon_op_timeout;
  r = cluster.conf_set("rados_mon_op_timeout", oss.str().c_str());
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  // Connect to cluster
  r = cluster.connect();
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  return 0;
}

} // namespace util
} // namespace librbd
