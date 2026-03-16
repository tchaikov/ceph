// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "BackfillDaemon.h"
#include "common/ceph_argparse.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/errno.h"
#include "global/global_init.h"
#include "global/signal_handler.h"

#include <vector>
#include <iostream>

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "rbd-backfill: "

namespace {

// Global pointer set before signal handlers are registered so handle_signal
// can call daemon.shutdown() — safe because Ceph's async signal handler runs
// the callback in a dedicated thread (not the signal context).
static rbd::backfill::BackfillDaemon* g_daemon = nullptr;

void handle_signal(int signum) {
  if (g_daemon) {
    g_daemon->shutdown();
  }
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
  std::cout << "  --foreground            Run in foreground (logs to stderr; default when" << std::endl;
  std::cout << "                          not daemonized)" << std::endl;
  std::cout << "  -c, --conf <file>       Configuration file (REQUIRED)" << std::endl;
  std::cout << "  --log-file <file>       Log file path" << std::endl;
  std::cout << "  --debug <subsys> <lvl>  Set debug level (e.g. --debug rbd 20)" << std::endl;
  std::cout << std::endl;
  std::cout << "Example:" << std::endl;
  std::cout << "  # Schedule images for backfill" << std::endl;
  std::cout << "  rbd backfill schedule rbd/parent1" << std::endl;
  std::cout << "  rbd backfill schedule rbd/parent2" << std::endl;
  std::cout << std::endl;
  std::cout << "  # Start in foreground with debug output visible on terminal" << std::endl;
  std::cout << "  rbd-backfill --foreground --debug rbd 20 --conf /etc/ceph/ceph.conf" << std::endl;
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

  // Handle --help (must check before common_init_finish so output goes to stdout)
  for (auto& arg : args) {
    if (std::string(arg) == "-h" || std::string(arg) == "--help") {
      usage();
      return 0;
    }
  }

  // When --foreground is passed, global_init sets daemonize=false.
  // In that case also redirect logs to stderr so users see output on the terminal.
  // (CODE_ENVIRONMENT_DAEMON defaults to logging to a file, not stderr.)
  if (!g_conf()->daemonize) {
    g_ceph_context->_conf.set_val("log_to_stderr", "true");
    g_ceph_context->_conf.apply_changes(nullptr);
  }

  // Initialize logging
  common_init_finish(g_ceph_context);

  // Daemonize if not running in foreground mode (standard Ceph daemon pattern;
  // mirrors src/tools/rbd_mirror/main.cc — do not add custom --daemon/--foreground
  // parsing here; global_init already handles those flags via conf.parse_argv()).
  if (g_conf()->daemonize) {
    global_init_daemonize(g_ceph_context);
  }

  dout(0) << "rbd-backfill starting (will discover images via metadata)" << dendl;

  // Install signal handlers
  init_async_signal_handler();
  register_async_signal_handler(SIGHUP, sighup_handler);
  register_async_signal_handler_oneshot(SIGINT, handle_signal);
  register_async_signal_handler_oneshot(SIGTERM, handle_signal);

  // Create and run daemon
  int r = 0;
  {
    rbd::backfill::BackfillDaemon daemon(g_ceph_context);
    g_daemon = &daemon;  // allow signal handler to call daemon.shutdown()

    r = daemon.init();
    if (r < 0) {
      derr << "failed to initialize daemon: " << cpp_strerror(r) << dendl;
      g_daemon = nullptr;
      goto cleanup;
    }

    dout(0) << "daemon initialized, starting main loop" << dendl;
    daemon.run();

    g_daemon = nullptr;
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
