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
  std::cout << "Usage: rbd-backfill [options]" << std::endl;
  std::cout << std::endl;
  std::cout << "Background S3 backfill daemon for RBD standalone parent images" << std::endl;
  std::cout << std::endl;
  std::cout << "The daemon automatically discovers and backfills images that have been" << std::endl;
  std::cout << "scheduled for backfill using 'rbd backfill schedule <image>'." << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  --daemon                Run in daemon mode (default: foreground)" << std::endl;
  std::cout << "  --foreground            Run in foreground mode" << std::endl;
  std::cout << "  -c, --conf <file>       Configuration file" << std::endl;
  std::cout << "  --log-file <file>       Log file path" << std::endl;
  std::cout << std::endl;
  std::cout << "Example:" << std::endl;
  std::cout << "  # Schedule images for backfill" << std::endl;
  std::cout << "  rbd backfill schedule rbd/parent1" << std::endl;
  std::cout << "  rbd backfill schedule rbd/parent2" << std::endl;
  std::cout << std::endl;
  std::cout << "  # Start the daemon (will discover scheduled images)" << std::endl;
  std::cout << "  rbd-backfill --foreground" << std::endl;
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
  bool daemon_mode = false;

  for (auto i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_flag(args, i, "-h", "--help", (char*)nullptr)) {
      usage();
      return 0;
    } else if (ceph_argparse_flag(args, i, "--daemon", (char*)nullptr)) {
      daemon_mode = true;
    } else if (ceph_argparse_flag(args, i, "--foreground", (char*)nullptr)) {
      daemon_mode = false;
    } else {
      ++i;
    }
  }

  // Initialize logging
  common_init_finish(g_ceph_context);

  dout(0) << "rbd-backfill starting (will discover images via metadata)" << dendl;

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
    rbd::backfill::BackfillDaemon daemon(g_ceph_context);

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
