// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include <iostream>

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>

#include "common/ceph_argparse.h"
#include "crimson/common/config_proxy.h"
#include "crimson/mon/MonClient.h"
#include "crimson/net/SocketMessenger.h"

#include "osd.h"

using config_t = ceph::common::ConfigProxy;

void usage() {
  std::cout << "usage: crimson -i <ID>" << std::endl;
  generic_server_usage();
}

int main(int argc, char* argv[])
{
  std::vector<const char*> args{argv + 1, argv + argc};
  if (args.empty()) {
    std::cerr << argv[0] << ": -h or --help for usage" << std::endl;
    return EXIT_FAILURE;
  }
  std::string cluster;
  std::string conf_file_list;
  // ceph_argparse_early_args() could _exit(), while local_conf() won't ready
  // until it's started. so do the boilerplate-settings parsing here.
  auto init_params = ceph_argparse_early_args(args,
                                              CEPH_ENTITY_TYPE_CLIENT,
                                              &cluster,
                                              &conf_file_list);
  if (ceph_argparse_need_usage(args)) {
    usage();
    return EXIT_SUCCESS;
  }

  int return_value = 0;
  // TODO: add heartbeat
  seastar::app_template app;
  // talk with osd
  ceph::net::SocketMessenger cluster_msgr;
  // talk with mon/mgr
  ceph::net::SocketMessenger client_msgr;
  ceph::mon::Client monc{client_msgr};
  seastar::sharded<OSD> osd;

  try {
    return app.run(argc, argv, [&] {
        return seastar::async([&cluster_msgr, &client_msgr, &monc, &osd,
                               &init_params, &cluster, &conf_file_list,
                               &return_value] {
        ceph::common::sharded_conf().start(init_params.name, cluster).get();
        seastar::engine().at_exit([] {
          return ceph::common::sharded_conf().stop();
        });
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
        monc.set_name(conf->name);
        monc.build_initial_map().get();
        monc.load_keyring().get();
        client_msgr.start(&monc);
        seastar::engine().at_exit([&client_msgr] {
          return client_msgr.shutdown();
        });
        monc.authenticate().get();
        seastar::engine().at_exit([&monc] {
          return monc.stop();
        });
        osd.start().get();
        seastar::engine().at_exit([&osd, &return_value] {
          return osd.stop().then([&return_value] {
            ::_exit(return_value);
          });
        });
      }).then_wrapped([&return_value] (auto&& f) {
        try {
          f.get();
        } catch (...) {
          return_value = 1;
          seastar::engine_exit(std::current_exception());
        }
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
