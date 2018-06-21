// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include <chrono>
#include <numeric>
#include <core/app-template.hh>
#include "crimson/thread/ThreadPool.h"
#include "crimson/net/SocketMessenger.h"
int main()
{
  seastar::app_template app;
  ceph::net::SocketMessenger ms_public{entity_name_t::OSD(whoami)};
  ceph::net::SocketMessenger ms_cluster{entity_name_t::OSD(whoami)};

  map<string, string> defaults = {
    // We want to enable leveldb's log, while allowing users to override this
    // option, therefore we will pass it as a default argument to global_init().
    { "leveldb_log", "" }
  };
  auto cct = global_init(
    &defaults,
    args, CEPH_ENTITY_TYPE_OSD,
    CODE_ENVIRONMENT_DAEMON,
    0, "osd_data");

  srand(time(NULL) + getpid());

  MonClient mc(cct.get());
  if (mc.build_initial_monmap() < 0)
    return -1;
  global_init_chdir(cct.get());
  if (global_init_preload_erasure_code(cct.get()) < 0) {
    return 1;
  }
  return app.run(argc, argv, [&] {
                              
  });
}
