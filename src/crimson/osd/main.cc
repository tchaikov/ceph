// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include <sys/types.h>
#include <unistd.h>

#include <iostream>

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>

#include "common/ceph_argparse.h"
#include "crimson/common/config_proxy.h"
#include "crimson/net/SocketMessenger.h"

#include "osd.h"

using config_t = ceph::common::ConfigProxy;

void usage(const char* prog) {
  std::cout << "usage: " << prog << " -i <ID>" << std::endl;
  generic_server_usage();
}

int main(int argc, char* argv[])
{
  std::vector<const char*> args{argv + 1, argv + argc};
  if (ceph_argparse_need_usage(args)) {
    usage(argv[0]);
    return EXIT_SUCCESS;
  }
  std::string cluster;
  std::string conf_file_list;
  // ceph_argparse_early_args() could _exit(), while local_conf() won't ready
  // until it's started. so do the boilerplate-settings parsing here.
  auto init_params = ceph_argparse_early_args(args,
                                              CEPH_ENTITY_TYPE_OSD,
                                              &cluster,
                                              &conf_file_list);
  int return_value = 0;
  // TODO: add heartbeat
  seastar::app_template app;
  // talk with osd
  ceph::net::SocketMessenger cluster_msgr("cluster", getpid());
  // talk with mon/mgr
  ceph::net::SocketMessenger client_msgr("client", getpid());
  seastar::sharded<OSD> osd;

  args.insert(begin(args), argv[0]);
  try {
    return app.run_deprecated(args.size(), const_cast<char**>(args.data()), [&] {
        return seastar::async([&cluster_msgr, &client_msgr, &osd,
                               &init_params, &cluster, &conf_file_list,
                               &return_value] {
        ceph::common::sharded_conf().start(init_params.name, cluster).get();
        ceph::common::sharded_perf_coll().start().get();
        auto& conf = ceph::common::local_conf();
        conf.parse_config_files(conf_file_list).get();
        const auto whoami = std::stoi(conf->name.get_id());
        for (ceph::net::SocketMessenger& msgr : {std::ref(cluster_msgr),
                                                 std::ref(client_msgr)}) {
          msgr.set_myname(entity_name_t::OSD(whoami));
          if (conf->ms_crc_data) {
            msgr.set_crc_data();
          }
          if (conf->ms_crc_header) {
            msgr.set_crc_header();
          }
        }

        osd.start_single(std::stoi(conf->name.get_id()),
                         std::ref(cluster_msgr),
                         std::ref(client_msgr)).then([&] {
          return osd.invoke_on(0, &OSD::start);
        }).get();

        seastar::engine().at_exit([] {
          return ceph::common::sharded_conf().stop();
        });
        seastar::engine().at_exit([] {
          return ceph::common::sharded_perf_coll().stop();
        });
        seastar::engine().at_exit([&] {
          return client_msgr.shutdown();
        });
        seastar::engine().at_exit([&] {
          return osd.stop();
        });
      });
    });
  } catch (...) {
    seastar::fprint(std::cerr, "FATAL: Exception during startup, aborting: %s\n", std::current_exception());
    return EXIT_FAILURE;
  }
}

/*
 * Local Variables:
 * compile-command: "make -j4 \
 * -C ../../../build \
 * crimson-osd"
 * End:
 */
