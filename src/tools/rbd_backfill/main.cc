// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "BackfillDaemon.h"
#include "common/ceph_argparse.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/errno.h"
#include "global/global_init.h"
#include "global/signal_handler.h"

#include <atomic>
#include <vector>
#include <iostream>

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd-backfill: "

namespace {

// Global flag to track if we've already shut down
static std::atomic<bool> shutdown_initiated{false};

void handle_signal(int signum) {
  // Don't call shutdown_async_signal_handler() here
  // It will be called in the cleanup section of main()
  // Just set a flag or do minimal work
}

void usage() {
  std::cout << "Usage: rbd-backfill [options] --pool <pool> --image <image> [--pool <pool> --image <image> ...]" << std::endl;
  std::cout << std::endl;
  std::cout << "Background S3 backfill daemon for RBD standalone parent images" << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  --pool <pool>           Pool containing parent image to backfill" << std::endl;
  std::cout << "  --image <image>         Parent image name to backfill" << std::endl;
  std::cout << "  --daemon                Run in daemon mode (default: foreground)" << std::endl;
  std::cout << "  --foreground            Run in foreground mode" << std::endl;
  std::cout << "  -c, --conf <file>       Configuration file" << std::endl;
  std::cout << "  --log-file <file>       Log file path" << std::endl;
  std::cout << std::endl;
  std::cout << "Example:" << std::endl;
  std::cout << "  rbd-backfill --pool rbd --image parent1 --pool rbd --image parent2" << std::endl;
  std::cout << std::endl;
}

} // anonymous namespace

using ImageSpec = rbd::backfill::ImageSpec;

int main(int argc, const char **argv) {
  std::vector<const char*> args;
  argv_to_vec(argc, argv, args);

  if (args.empty()) {
    usage();
    return 0;
  }

  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_DAEMON,
                         CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS);

  // Parse command-line arguments
  std::vector<ImageSpec> images_to_backfill;
  bool daemon_mode = false;
  std::string current_pool;
  std::string current_image;

  for (auto i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_flag(args, i, "-h", "--help", (char*)nullptr)) {
      usage();
      return 0;
    } else if (ceph_argparse_witharg(args, i, &current_pool, "--pool", (char*)nullptr)) {
      // Pool name stored
    } else if (ceph_argparse_witharg(args, i, &current_image, "--image", (char*)nullptr)) {
      if (current_pool.empty()) {
        std::cerr << "Error: --image must follow --pool" << std::endl;
        return EXIT_FAILURE;
      }
      images_to_backfill.push_back({current_pool, current_image});
      current_pool.clear();
      current_image.clear();
    } else if (ceph_argparse_flag(args, i, "--daemon", (char*)nullptr)) {
      daemon_mode = true;
    } else if (ceph_argparse_flag(args, i, "--foreground", (char*)nullptr)) {
      daemon_mode = false;
    } else {
      ++i;
    }
  }

  if (images_to_backfill.empty()) {
    std::cerr << "Error: No images specified for backfill" << std::endl;
    std::cerr << "Use --pool <pool> --image <image> to specify images" << std::endl;
    usage();
    return EXIT_FAILURE;
  }

  // Initialize logging
  common_init_finish(g_ceph_context);

  dout(0) << "rbd-backfill starting" << dendl;
  dout(5) << "Images to backfill: " << images_to_backfill.size() << dendl;
  for (const auto& spec : images_to_backfill) {
    dout(5) << "  - " << spec.pool_name << "/" << spec.image_name << dendl;
  }

  // Daemonize if requested
  if (daemon_mode) {
    dout(0) << "entering daemon mode" << dendl;
    global_init_daemonize(g_ceph_context);
  }

  // Install signal handlers
  init_async_signal_handler();
  register_async_signal_handler(SIGHUP, sighup_handler);
  register_async_signal_handler_oneshot(SIGINT, handle_signal);
  register_async_signal_handler_oneshot(SIGTERM, handle_signal);

  // Create and run daemon
  int r = 0;
  {
    rbd::backfill::BackfillDaemon daemon(g_ceph_context, images_to_backfill);

    r = daemon.init();
    if (r < 0) {
      derr << "failed to initialize daemon: " << cpp_strerror(r) << dendl;
      goto cleanup;
    }

    dout(0) << "daemon initialized, starting main loop" << dendl;
    daemon.run();

    dout(0) << "shutting down daemon" << dendl;
    daemon.shutdown();
  }

cleanup:
  unregister_async_signal_handler(SIGHUP, sighup_handler);
  unregister_async_signal_handler(SIGINT, handle_signal);
  unregister_async_signal_handler(SIGTERM, handle_signal);
  shutdown_async_signal_handler();

  dout(0) << "rbd-backfill stopped" << dendl;

  return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
