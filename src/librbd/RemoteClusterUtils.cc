// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/RemoteClusterUtils.h"
#include "common/ceph_context.h"
#include "common/config.h"
#include "common/ConfUtils.h"
#include "common/dout.h"
#include "common/errno.h"
#include "include/encoding.h"
#include "include/stringify.h"
#include <boost/algorithm/string.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::RemoteClusterUtils: "

namespace librbd {
namespace util {

int parse_mon_hosts_from_config(const std::string& conf_path,
                                std::vector<std::string>& mon_hosts,
                                std::string& cluster_name) {
  ConfFile cf;
  std::deque<std::string> errors;
  int r = cf.parse_file(conf_path, &errors, nullptr);
  if (r < 0) {
    return r;
  }

  std::string mon_host_str;
  // Try [global] first, then [mon] — same search order as the Ceph config manager
  if (cf.read("global", "mon_host", mon_host_str) < 0 &&
      cf.read("mon", "mon_host", mon_host_str) < 0) {
    return -EINVAL;
  }

  // Derive cluster name from the basename prefix before ".conf"
  auto start = conf_path.rfind('/') + 1;
  auto end = conf_path.find(".conf", start);
  cluster_name = (end == std::string::npos) ? "ceph"
                                            : conf_path.substr(start, end - start);

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
  ConfFile cf;
  std::deque<std::string> errors;
  int r = cf.parse_file(keyring_path, &errors, nullptr);
  if (r < 0) {
    return (r == -ENOENT) ? -ENOENT : -EINVAL;
  }

  std::string key_value;
  r = cf.read(client_name, "key", key_value);
  if (r < 0 || key_value.empty()) {
    return -EINVAL;
  }

  // Key is already base64-encoded in keyring file, no need to encode again
  encoded_keyring = std::move(key_value);
  return 0;
}

int connect_to_remote_cluster(CephContext* cct,
                              const std::string& cluster_name,
                              const std::vector<std::string>& mon_hosts,
                              const std::string& keyring,
                              const std::string& client_name,
                              librados::Rados& cluster) {
  int r;

  r = cluster.init2(client_name.c_str(), cluster_name.c_str(), 0);
  if (r < 0) {
    return r;
  }

  std::string mon_host_str = joinify(mon_hosts.begin(), mon_hosts.end(),
                                     std::string(","));
  auto& conf = cct->_conf;

  // conf_set avoids creating a temporary keyring file, eliminating the
  // window where key material would exist on disk.
  auto set = [&](const char* key, const std::string& value) {
    int rc = cluster.conf_set(key, value.c_str());
    if (rc < 0) {
      cluster.shutdown();
    }
    return rc;
  };
  if ((r = set("mon_host",             mon_host_str)) < 0 ||
      (r = set("key",                  keyring))      < 0 ||
      (r = set("keyring",              ""))           < 0 ||
      (r = set("rados_osd_op_timeout", stringify(conf->rados_osd_op_timeout))) < 0 ||
      (r = set("client_mount_timeout", stringify(conf->client_mount_timeout))) < 0 ||
      (r = set("rados_mon_op_timeout", stringify(conf->rados_mon_op_timeout))) < 0) {
    return r;
  }

  r = cluster.connect();
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  return 0;
}

int open_remote_parent_ioctx(CephContext* cct,
                              const RemoteParentSpec& remote,
                              const std::string& pool_name,
                              int64_t pool_id,
                              const std::string& pool_namespace,
                              librados::Rados& cluster,
                              librados::IoCtx& ioctx) {
  int r = connect_to_remote_cluster(cct,
    remote.cluster_name, remote.mon_hosts, remote.keyring,
    DEFAULT_REMOTE_CLIENT_NAME, cluster);
  if (r < 0) {
    return r;
  }

  if (!pool_name.empty()) {
    r = cluster.ioctx_create(pool_name.c_str(), ioctx);
  } else {
    r = cluster.ioctx_create2(pool_id, ioctx);
  }
  if (r < 0) {
    cluster.shutdown();
    return r;
  }

  if (!pool_namespace.empty()) {
    ioctx.set_namespace(pool_namespace);
  }
  return 0;
}

} // namespace util
} // namespace librbd
