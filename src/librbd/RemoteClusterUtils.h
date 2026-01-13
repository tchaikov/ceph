// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_REMOTE_CLUSTER_UTILS_H
#define CEPH_LIBRBD_REMOTE_CLUSTER_UTILS_H

#include <string>
#include <vector>
#include "include/rados/librados.hpp"

class CephContext;

namespace librbd {
namespace util {

/**
 * Parse monitor addresses from ceph.conf file
 *
 * @param conf_path Path to ceph.conf file
 * @param mon_hosts Output vector of monitor addresses
 * @param cluster_name Output cluster name (optional)
 * @return 0 on success, negative error code on failure
 */
int parse_mon_hosts_from_config(const std::string& conf_path,
                                std::vector<std::string>& mon_hosts,
                                std::string& cluster_name);

/**
 * Read keyring file and extract key
 *
 * @param keyring_path Path to keyring file
 * @param client_name Client name (e.g., "client.admin")
 * @param encoded_keyring Output base64-encoded key (as stored in keyring file)
 * @return 0 on success, negative error code on failure
 *
 * Note: Keys in keyring files are already base64-encoded, so this function
 * simply extracts the key value without additional encoding.
 */
int read_and_encode_keyring(const std::string& keyring_path,
                            const std::string& client_name,
                            std::string& encoded_keyring);

/**
 * Create RADOS connection to remote cluster
 *
 * @param cct CephContext for reading current cluster's configuration
 * @param cluster_name Remote cluster name
 * @param mon_hosts Vector of monitor addresses
 * @param keyring Base64-encoded keyring
 * @param client_name Client name (e.g., "client.admin")
 * @param cluster Output RADOS cluster handle
 * @return 0 on success, negative error code on failure
 *
 * Note: Keyring is set directly in memory (no temporary files created)
 * Timeout settings are inherited from current cluster configuration
 */
int connect_to_remote_cluster(CephContext* cct,
                              const std::string& cluster_name,
                              const std::vector<std::string>& mon_hosts,
                              const std::string& keyring,
                              const std::string& client_name,
                              librados::Rados& cluster);

} // namespace util
} // namespace librbd

#endif // CEPH_LIBRBD_REMOTE_CLUSTER_UTILS_H
