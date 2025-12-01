// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include "common/Formatter.h"
#include "common/TextTable.h"
#include "include/stringify.h"
#include <iostream>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace backfill {

namespace at = argument_types;
namespace po = boost::program_options;

// Metadata key for backfill scheduling
static const std::string BACKFILL_SCHEDULED_KEY = "backfill_scheduled";
static const std::string BACKFILL_STATUS_KEY = "backfill_status";

namespace {

int validate_s3_backed_image(librbd::Image& image) {
  std::string value;
  int r = image.metadata_get("s3.bucket", &value);
  if (r < 0) {
    std::cerr << "rbd: image is not S3-backed (no s3.bucket metadata)" << std::endl;
    return r;
  }
  return 0;
}

} // anonymous namespace

void get_schedule_arguments(po::options_description *positional,
                            po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
}

int execute_schedule(const po::variables_map &vm,
                    const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
      vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &namespace_name,
      &image_name, &snap_name, true, utils::SNAPSHOT_PRESENCE_NONE,
      utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, namespace_name, image_name, "", "",
                                 false, &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  // Validate that this is an S3-backed image
  r = validate_s3_backed_image(image);
  if (r < 0) {
    return r;
  }

  // Set metadata to mark image as scheduled for backfill
  std::string timestamp = stringify(time(nullptr));
  r = image.metadata_set(BACKFILL_SCHEDULED_KEY, "true");
  if (r < 0) {
    std::cerr << "rbd: failed to schedule backfill: " << cpp_strerror(r) << std::endl;
    return r;
  }

  r = image.metadata_set(BACKFILL_STATUS_KEY, "scheduled");
  if (r < 0) {
    std::cerr << "rbd: failed to set backfill status: " << cpp_strerror(r) << std::endl;
    return r;
  }

  std::cout << "Backfill scheduled for " << pool_name << "/"
            << (namespace_name.empty() ? "" : namespace_name + "/")
            << image_name << std::endl;
  std::cout << "The rbd-backfill daemon will automatically start backfilling this image." << std::endl;

  return 0;
}

void get_list_arguments(po::options_description *positional,
                       po::options_description *options) {
  at::add_pool_options(positional, options, true);
  at::add_format_options(options);
}

int execute_list(const po::variables_map &vm,
                const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  int r = utils::get_pool_and_namespace_names(vm, true, false, &pool_name,
                                              &namespace_name, &arg_index);
  if (r < 0) {
    return r;
  }

  at::Format::Formatter formatter;
  r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, namespace_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  std::vector<librbd::image_spec_t> images;
  r = rbd.list2(io_ctx, &images);
  if (r < 0) {
    std::cerr << "rbd: failed to list images: " << cpp_strerror(r) << std::endl;
    return r;
  }

  TextTable tbl;
  bool has_entries = false;
  if (formatter.get()) {
    formatter->open_array_section("images");
  } else {
    tbl.define_column("IMAGE", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("STATUS", TextTable::LEFT, TextTable::LEFT);
  }

  for (const auto& image_spec : images) {
    librbd::Image image;
    r = rbd.open(io_ctx, image, image_spec.name.c_str());
    if (r < 0) {
      continue;
    }

    std::string scheduled_value;
    r = image.metadata_get(BACKFILL_SCHEDULED_KEY, &scheduled_value);
    if (r >= 0 && scheduled_value == "true") {
      std::string status_value = "unknown";
      image.metadata_get(BACKFILL_STATUS_KEY, &status_value);

      if (formatter.get()) {
        formatter->open_object_section("image");
        formatter->dump_string("name", image_spec.name);
        formatter->dump_string("status", status_value);
        formatter->close_section();
      } else {
        tbl << image_spec.name << status_value << TextTable::endrow;
        has_entries = true;
      }
    }
    image.close();
  }

  if (formatter.get()) {
    formatter->close_section();
    formatter->flush(std::cout);
  } else {
    if (has_entries) {
      std::cout << tbl;
    }
  }

  return 0;
}

void get_status_arguments(po::options_description *positional,
                         po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
  at::add_format_options(options);
}

int execute_status(const po::variables_map &vm,
                  const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
      vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &namespace_name,
      &image_name, &snap_name, true, utils::SNAPSHOT_PRESENCE_NONE,
      utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  at::Format::Formatter formatter;
  r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, namespace_name, image_name, "", "",
                                 false, &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  std::string scheduled_value;
  r = image.metadata_get(BACKFILL_SCHEDULED_KEY, &scheduled_value);
  if (r < 0 || scheduled_value != "true") {
    std::cerr << "rbd: backfill not scheduled for this image" << std::endl;
    return -EINVAL;
  }

  std::string status_value = "unknown";
  image.metadata_get(BACKFILL_STATUS_KEY, &status_value);

  if (formatter.get()) {
    formatter->open_object_section("backfill_status");
    formatter->dump_string("image", pool_name + "/" +
                          (namespace_name.empty() ? "" : namespace_name + "/") +
                          image_name);
    formatter->dump_string("scheduled", scheduled_value);
    formatter->dump_string("status", status_value);
    formatter->close_section();
    formatter->flush(std::cout);
  } else {
    std::cout << "Image: " << pool_name << "/"
              << (namespace_name.empty() ? "" : namespace_name + "/")
              << image_name << std::endl;
    std::cout << "Backfill scheduled: " << scheduled_value << std::endl;
    std::cout << "Backfill status: " << status_value << std::endl;
  }

  return 0;
}

void get_cancel_arguments(po::options_description *positional,
                         po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
}

int execute_cancel(const po::variables_map &vm,
                  const std::vector<std::string> &ceph_global_init_args) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string namespace_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
      vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &namespace_name,
      &image_name, &snap_name, true, utils::SNAPSHOT_PRESENCE_NONE,
      utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, namespace_name, image_name, "", "",
                                 false, &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  // Remove backfill scheduling metadata
  r = image.metadata_remove(BACKFILL_SCHEDULED_KEY);
  if (r < 0 && r != -ENOENT) {
    std::cerr << "rbd: failed to cancel backfill: " << cpp_strerror(r) << std::endl;
    return r;
  }

  r = image.metadata_remove(BACKFILL_STATUS_KEY);
  if (r < 0 && r != -ENOENT) {
    std::cerr << "rbd: failed to remove backfill status: " << cpp_strerror(r) << std::endl;
    return r;
  }

  std::cout << "Backfill canceled for " << pool_name << "/"
            << (namespace_name.empty() ? "" : namespace_name + "/")
            << image_name << std::endl;

  return 0;
}

Shell::Action action_schedule(
  {"backfill", "schedule"}, {},
  "Schedule backfill for S3-backed parent image.",
  "",
  &get_schedule_arguments, &execute_schedule);

Shell::Action action_list(
  {"backfill", "list"}, {"backfill", "ls"},
  "List images scheduled for backfill.",
  "",
  &get_list_arguments, &execute_list);

Shell::Action action_status(
  {"backfill", "status"}, {},
  "Show backfill status for an image.",
  "",
  &get_status_arguments, &execute_status);

Shell::Action action_cancel(
  {"backfill", "cancel"}, {},
  "Cancel scheduled backfill for an image.",
  "",
  &get_cancel_arguments, &execute_cancel);

} // namespace backfill
} // namespace action
} // namespace rbd
