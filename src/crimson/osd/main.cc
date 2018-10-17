// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#incldue <iostream>
#include <seastar/core/app-template.hh>
#include "common/ceph_argparse.h"
#include "crimson/common/config_proxy.h"
#include "crimson/mon/MonClient.h"
#include "crimson/net/SocketMessenger.h"

using config_t = ceph::common::ConfigProxy;

namespace {

void usage() {
  std::cout << "usage: crimson -i <ID>" << std::endl;
  generic_server_usage();
}

}

int main(int argc, const char* argv[])
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

  // TODO: add heartbeat
  seastar::app_template app;
  return app.run(argc, argv, [&] {
    return seastar::async([] {
      ceph::common::sharded_conf().start().get();
      engine().at_exit([] {
        ceph::common::sharded_conf().stop();
      });
      auto& conf = ceph::common::local_conf();
      conf->name = init_params.name;
      conf->cluster = cluster;
      conf.parse_config_files(conf_file_list).get();
      auto& conf = ceph::common::local_conf();
      const auto whoami = std::stoi(conf->name.get_id());
      // talk with osd
      ceph::net::SocketMessenger cluster_msgr{entity_name_t::OSD(whoami)};
      // talk with mon/mgr
      ceph::net::SocketMessenger client_msgr{entity_name_t::OSD(whoami)};
      if (conf->ms_crc_data) {
	cluster_msgr.set_crc_data();
	client_msgr.set_crc_data();
      }
      if (conf->ms_crc_header) {
	cluster_msgr.set_crc_header();
	client_msgr.set_crc_header();
      }
      ceph::mon::Client monc{conf->name, client_msgr};
      monc.build_initial_map().get();
      monc.load_keyring().get();
      client_msgr.start(&monc);
      engine().at_exit([&monc] {
        monc.stop();
      });
      monc.authenticate().get();
      engine().at_exit([&monc] {
        monc.stop();
      });
    test_monc().then([] {
      std::cout << "All tests succeeded" << std::endl;
    }).handle_exception([] (auto eptr) {
      std::cout << "Test failure" << std::endl;
      return seastar::make_exception_future<>(eptr);
    });
  });
}

/*
 * Local Variables:
 * compile-command: "make -j4 \
 * -C ../../../build \
 * crimson"
 * End:
 */
