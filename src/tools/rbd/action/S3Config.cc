// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include "include/buffer.h"
#include <iostream>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace s3_config {

namespace at = argument_types;
namespace po = boost::program_options;

static int set_metadata(librbd::Image &image, const std::string &key,
                        const std::string &value) {
  return image.metadata_set(key, value);
}

static int get_metadata(librbd::Image &image, const std::string &key,
                        std::string *value) {
  return image.metadata_get(key, value);
}

static std::string base64_encode(const std::string &input) {
  ceph::bufferlist bl;
  bl.append(input);
  ceph::bufferlist encoded;
  bl.encode_base64(encoded);
  return encoded.to_str();
}

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
    {"s3.enabled", "true"},
    {"s3.bucket", s3_bucket},
    {"s3.endpoint", s3_endpoint},
    {"s3.region", s3_region},
    {"s3.prefix", s3_prefix},
    {"s3.image_name", s3_image_name},
    {"s3.image_format", s3_image_format},
    {"s3.timeout_ms", std::to_string(s3_timeout_ms)},
    {"s3.max_retries", std::to_string(s3_max_retries)}
  };

  // Add credentials if provided (base64-encode secret key for storage)
  if (has_access_key) {
    metadata["s3.access_key"] = s3_access_key;
    metadata["s3.secret_key"] = base64_encode(s3_secret_key);
  }

  // Set all metadata
  for (const auto& kv : metadata) {
    r = set_metadata(image, kv.first, kv.second);
    if (r < 0) {
      std::cerr << "rbd: error setting metadata " << kv.first << ": "
                << cpp_strerror(r) << std::endl;
      return r;
    }
  }

  std::cout << "S3 configuration set successfully for image " << image_name << std::endl;
  if (!has_access_key) {
    std::cout << "Note: No credentials provided, using anonymous access" << std::endl;
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

  // Get S3 configuration metadata
  std::vector<std::string> keys = {
    "s3.enabled", "s3.bucket", "s3.endpoint", "s3.region",
    "s3.access_key", "s3.secret_key", "s3.prefix",
    "s3.image_name", "s3.image_format", "s3.timeout_ms", "s3.max_retries"
  };

  std::string enabled;
  r = get_metadata(image, "s3.enabled", &enabled);
  if (r < 0 || (enabled != "true" && enabled != "1")) {
    std::cout << "S3 configuration is not set for image " << image_name << std::endl;
    return 0;
  }

  std::cout << "S3 configuration for image " << image_name << ":" << std::endl;

  for (const auto& key : keys) {
    std::string value;
    r = get_metadata(image, key, &value);
    if (r == 0) {
      // Mask secret key for display
      if (key == "s3.secret_key" && !value.empty()) {
        std::cout << "  " << key << ": ********" << std::endl;
      } else {
        std::cout << "  " << key << ": " << value << std::endl;
      }
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
    "s3.enabled", "s3.bucket", "s3.endpoint", "s3.region",
    "s3.access_key", "s3.secret_key", "s3.prefix",
    "s3.image_name", "s3.image_format", "s3.timeout_ms", "s3.max_retries"
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
