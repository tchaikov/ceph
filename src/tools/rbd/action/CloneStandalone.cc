// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include "librbd/RemoteClusterUtils.h"
#include <iostream>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace clone_standalone {

namespace at = argument_types;
namespace po = boost::program_options;

int do_clone_standalone(librbd::RBD &rbd, librados::IoCtx &p_ioctx,
                        const char *p_name, librados::IoCtx &c_ioctx,
                        const char *c_name, librbd::ImageOptions& opts) {
  return rbd.clone_standalone(p_ioctx, p_name, c_ioctx, c_name, opts);
}

int do_clone_standalone_remote(librbd::RBD &rbd, librados::IoCtx &p_ioctx,
                               const char *p_name, librados::IoCtx &c_ioctx,
                               const char *c_name, librbd::ImageOptions& opts,
                               const std::string& remote_cluster_conf,
                               const std::string& remote_keyring,
                               const std::string& remote_client_name) {
  return rbd.clone_standalone_remote(p_ioctx, p_name, c_ioctx, c_name, opts,
                                     remote_cluster_conf, remote_keyring,
                                     remote_client_name);
}

void get_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_SOURCE);
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_DEST);
  at::add_create_image_options(options, false);

  // Remote cluster options
  options->add_options()
    ("remote-cluster-conf", po::value<std::string>(),
     "path to remote cluster configuration file")
    ("remote-keyring", po::value<std::string>(),
     "path to remote cluster keyring file (optional, defaults to keyring in conf directory)")
    ("remote-client-name", po::value<std::string>()->default_value("client.admin"),
     "client name for remote cluster authentication");
}

int execute(const po::variables_map &vm,
            const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_SOURCE, &arg_index, &pool_name, &namespace_name,
    &image_name, &snap_name, true, utils::SNAPSHOT_PRESENCE_NONE,
    utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  if (!snap_name.empty()) {
    std::cerr << "rbd: snapshot name not allowed for standalone clone (cloning from mutable parent)"
              << std::endl;
    return -EINVAL;
  }

  std::string dst_pool_name;
  std::string dst_namespace_name;
  std::string dst_image_name;
  std::string dst_snap_name;
  r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_DEST, &arg_index, &dst_pool_name,
    &dst_namespace_name, &dst_image_name, &dst_snap_name, true,
    utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_FULL);
  if (r < 0) {
    return r;
  }

  librbd::ImageOptions opts;
  r = utils::get_image_options(vm, false, &opts);
  if (r < 0) {
    return r;
  }
  opts.set(RBD_IMAGE_OPTION_FORMAT, static_cast<uint64_t>(2));
  opts.set(RBD_IMAGE_OPTION_CLONE_FORMAT, static_cast<uint64_t>(2));

  // Check if this is a remote cluster clone
  bool is_remote = vm.count("remote-cluster-conf");
  std::string remote_cluster_conf;
  std::string remote_keyring;
  std::string remote_client_name = "client.admin";

  if (is_remote) {
    remote_cluster_conf = vm["remote-cluster-conf"].as<std::string>();

    if (vm.count("remote-keyring")) {
      remote_keyring = vm["remote-keyring"].as<std::string>();
    }

    if (vm.count("remote-client-name")) {
      remote_client_name = vm["remote-client-name"].as<std::string>();
    }
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, namespace_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librados::IoCtx dst_io_ctx;
  r = utils::init_io_ctx(rados, dst_pool_name, dst_namespace_name, &dst_io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;

  if (is_remote) {
    // Remote cluster clone
    r = do_clone_standalone_remote(rbd, io_ctx, image_name.c_str(), dst_io_ctx,
                                   dst_image_name.c_str(), opts,
                                   remote_cluster_conf, remote_keyring,
                                   remote_client_name);
  } else {
    // Local cluster clone
    r = do_clone_standalone(rbd, io_ctx, image_name.c_str(), dst_io_ctx,
                           dst_image_name.c_str(), opts);
  }

  if (r == -EXDEV) {
    std::cerr << "rbd: clone v2 required for cross-namespace clones."
              << std::endl;
    return r;
  } else if (r < 0) {
    std::cerr << "rbd: clone-standalone error: " << cpp_strerror(r) << std::endl;
    return r;
  }
  return 0;
}

Shell::Action action(
  {"clone-standalone"}, {},
  "Clone a mutable parent image into a CoW child image (no snapshot required).",
  at::get_long_features_help(), &get_arguments, &execute);

} // namespace clone_standalone
} // namespace action
} // namespace rbd
