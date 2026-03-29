// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include "include/buffer.h"
#include "include/stringify.h"
#include "librbd/Types.h"
#include <iostream>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace s3_config {

namespace at = argument_types;
namespace po = boost::program_options;


void get_set_arguments(po::options_description *positional,
                       po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);

  options->add_options()
    ("s3-bucket", po::value<std::string>(),
     "S3 bucket name containing the image data")
    ("s3-endpoint", po::value<std::string>(),
     "S3 endpoint URL (e.g., http://minio:9000)")
    ("s3-region", po::value<std::string>()->default_value("us-east-1"),
     "AWS region (default: us-east-1)")
    ("s3-access-key", po::value<std::string>(),
     "S3 access key ID for authentication")
    ("s3-secret-key", po::value<std::string>(),
     "S3 secret access key for authentication")
    ("s3-prefix", po::value<std::string>()->default_value(""),
     "S3 key prefix (optional)")
    ("s3-image-name", po::value<std::string>(),
     "name of the image object in S3 bucket")
    ("s3-image-format", po::value<std::string>()->default_value("raw"),
     "image format: raw or qcow2 (default: raw)")
    ("s3-timeout-ms", po::value<uint32_t>()->default_value(30000),
     "S3 request timeout in milliseconds (default: 30000)")
    ("s3-max-retries", po::value<uint32_t>()->default_value(3),
     "maximum number of retries for S3 requests (default: 3)");
}

int execute_set(const po::variables_map &vm,
                const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;

  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &namespace_name,
    &image_name, nullptr, true, utils::SNAPSHOT_PRESENCE_NONE,
    utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  // Validate required parameters
  if (!vm.count("s3-bucket")) {
    std::cerr << "rbd: --s3-bucket is required" << std::endl;
    return -EINVAL;
  }
  if (!vm.count("s3-endpoint")) {
    std::cerr << "rbd: --s3-endpoint is required" << std::endl;
    return -EINVAL;
  }
  if (!vm.count("s3-image-name")) {
    std::cerr << "rbd: --s3-image-name is required" << std::endl;
    return -EINVAL;
  }

  // Validate that access-key and secret-key are provided together
  bool has_access_key = vm.count("s3-access-key");
  bool has_secret_key = vm.count("s3-secret-key");
  if (has_access_key != has_secret_key) {
    std::cerr << "rbd: --s3-access-key and --s3-secret-key must be provided together"
              << std::endl;
    return -EINVAL;
  }

  std::string s3_bucket = vm["s3-bucket"].as<std::string>();
  std::string s3_endpoint = vm["s3-endpoint"].as<std::string>();
  std::string s3_region = vm["s3-region"].as<std::string>();
  std::string s3_access_key = has_access_key ? vm["s3-access-key"].as<std::string>() : "";
  std::string s3_secret_key = has_secret_key ? vm["s3-secret-key"].as<std::string>() : "";
  std::string s3_prefix = vm["s3-prefix"].as<std::string>();
  std::string s3_image_name = vm["s3-image-name"].as<std::string>();
  std::string s3_image_format = vm["s3-image-format"].as<std::string>();
  uint32_t s3_timeout_ms = vm["s3-timeout-ms"].as<uint32_t>();
  uint32_t s3_max_retries = vm["s3-max-retries"].as<uint32_t>();

  // Validate image format
  if (s3_image_format != "raw" && s3_image_format != "qcow2") {
    std::cerr << "rbd: --s3-image-format must be 'raw' or 'qcow2'" << std::endl;
    return -EINVAL;
  }

  // Connect to cluster and open image
  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, namespace_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  librbd::Image image;
  r = rbd.open(io_ctx, image, image_name.c_str());
  if (r < 0) {
    std::cerr << "rbd: error opening image " << image_name << ": "
              << cpp_strerror(r) << std::endl;
    return r;
  }

  // Set S3 configuration metadata
  std::map<std::string, std::string> metadata = {
    {librbd::S3_META_KEY_ENABLED,    "true"},
    {librbd::S3_META_KEY_BUCKET,     s3_bucket},
    {librbd::S3_META_KEY_ENDPOINT,   s3_endpoint},
    {librbd::S3_META_KEY_REGION,     s3_region},
    {librbd::S3_META_KEY_PREFIX,     s3_prefix},
    {librbd::S3_META_KEY_IMAGE_NAME, s3_image_name},
    {librbd::S3_META_KEY_IMAGE_FMT,  s3_image_format},
    {librbd::S3_META_KEY_TIMEOUT_MS, stringify(s3_timeout_ms)},
    {librbd::S3_META_KEY_MAX_RETRIES,stringify(s3_max_retries)},
  };

  // Add credentials if provided.
  // SECURITY NOTE: base64 is encoding, not encryption.  Both the access key
  // and the (base64-encoded) secret key are stored as plaintext RADOS image
  // metadata, readable by anyone with pool read access.  For production
  // deployments requiring stronger credential protection, rotate the S3
  // credentials regularly and restrict pool access using Ceph auth caps.
  if (has_access_key) {
    metadata[librbd::S3_META_KEY_ACCESS_KEY] = s3_access_key;
    ceph::bufferlist secret_bl; secret_bl.append(s3_secret_key);
    ceph::bufferlist secret_encoded; secret_bl.encode_base64(secret_encoded);
    metadata[librbd::S3_META_KEY_SECRET_KEY] = secret_encoded.to_str();
  }

  // Set all metadata
  for (const auto& kv : metadata) {
    r = image.metadata_set(kv.first, kv.second);
    if (r < 0) {
      std::cerr << "rbd: error setting metadata " << kv.first << ": "
                << cpp_strerror(r) << std::endl;
      return r;
    }
  }

  std::cout << "S3 configuration set successfully for image " << image_name << std::endl;
  if (!has_access_key) {
    std::cout << "Note: No credentials provided; S3 requests will be unsigned" << std::endl;
  }

  return 0;
}

void get_get_arguments(po::options_description *positional,
                       po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
}

int execute_get(const po::variables_map &vm,
                const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;

  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &namespace_name,
    &image_name, nullptr, true, utils::SNAPSHOT_PRESENCE_NONE,
    utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  // Connect to cluster and open image
  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, namespace_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  librbd::Image image;
  r = rbd.open(io_ctx, image, image_name.c_str());
  if (r < 0) {
    std::cerr << "rbd: error opening image " << image_name << ": "
              << cpp_strerror(r) << std::endl;
    return r;
  }

  // Fetch all S3 metadata in a single RADOS round-trip
  std::map<std::string, ceph::bufferlist> pairs;
  r = image.metadata_list("s3.", 20, &pairs);
  if (r < 0) {
    std::cerr << "rbd: error listing metadata: " << cpp_strerror(r) << std::endl;
    return r;
  }

  auto it = pairs.find(librbd::S3_META_KEY_ENABLED);
  if (it == pairs.end()) {
    std::cout << "S3 configuration is not set for image " << image_name << std::endl;
    return 0;
  }
  std::string enabled = it->second.to_str();
  if (enabled != "true" && enabled != "1") {
    std::cout << "S3 configuration is not set for image " << image_name << std::endl;
    return 0;
  }

  std::cout << "S3 configuration for image " << image_name << ":" << std::endl;

  for (const auto& kv : pairs) {
    if (kv.first.compare(0, 3, "s3.") != 0) {
      break;  // metadata_list is sorted; stop at first non-s3 key
    }
    std::string value = kv.second.to_str();
    // Mask secret key for display
    if (kv.first == librbd::S3_META_KEY_SECRET_KEY && !value.empty()) {
      std::cout << "  " << kv.first << ": ********" << std::endl;
    } else {
      std::cout << "  " << kv.first << ": " << value << std::endl;
    }
  }

  return 0;
}

void get_clear_arguments(po::options_description *positional,
                         po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
}

int execute_clear(const po::variables_map &vm,
                  const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;

  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &namespace_name,
    &image_name, nullptr, true, utils::SNAPSHOT_PRESENCE_NONE,
    utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  // Connect to cluster and open image
  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, namespace_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  librbd::Image image;
  r = rbd.open(io_ctx, image, image_name.c_str());
  if (r < 0) {
    std::cerr << "rbd: error opening image " << image_name << ": "
              << cpp_strerror(r) << std::endl;
    return r;
  }

  // Remove all S3 metadata keys
  std::vector<std::string> keys = {
    librbd::S3_META_KEY_ENABLED,    librbd::S3_META_KEY_BUCKET,
    librbd::S3_META_KEY_ENDPOINT,   librbd::S3_META_KEY_REGION,
    librbd::S3_META_KEY_ACCESS_KEY, librbd::S3_META_KEY_SECRET_KEY,
    librbd::S3_META_KEY_PREFIX,     librbd::S3_META_KEY_IMAGE_NAME,
    librbd::S3_META_KEY_IMAGE_FMT,  librbd::S3_META_KEY_TIMEOUT_MS,
    librbd::S3_META_KEY_MAX_RETRIES,
  };

  for (const auto& key : keys) {
    r = image.metadata_remove(key);
    // Ignore ENOENT (key doesn't exist)
    if (r < 0 && r != -ENOENT) {
      std::cerr << "rbd: error removing metadata " << key << ": "
                << cpp_strerror(r) << std::endl;
      return r;
    }
  }

  std::cout << "S3 configuration cleared for image " << image_name << std::endl;
  return 0;
}

Shell::Action action_set(
  {"s3-config", "set"}, {},
  "Set S3 configuration for an image (used for S3-backed parent images).",
  "", &get_set_arguments, &execute_set);

Shell::Action action_get(
  {"s3-config", "get"}, {},
  "Get S3 configuration for an image.",
  "", &get_get_arguments, &execute_get);

Shell::Action action_clear(
  {"s3-config", "clear"}, {},
  "Clear S3 configuration for an image.",
  "", &get_clear_arguments, &execute_clear);

} // namespace s3_config
} // namespace action
} // namespace rbd
