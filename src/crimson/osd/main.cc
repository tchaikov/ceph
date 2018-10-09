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

  seastar::app_template app;
  return app.run(argc, argv, [&] {
    return ceph::common::sharded_conf().start().then([] {
    auto& conf = ceph::common::local_conf();
    conf->name = init_params.name;
    conf->cluster = cluster;
    return conf.parse_config_files(conf_file_list);
  }).then([] {
      const entity_name_t whoami = entity_name_t::OSD(0);
      return seastar::do_with(ceph::net::SocketMessenger{whoami}, // talk to osd
                              ceph::net::SocketMessenger{whoami}, // talk to mon/mgr
                              // TODO: add heartbeat
        [](ceph::net::Messenger& cluster_msgr,
           ceph::net::Messenger& client_msgr) {
        auto& conf = ceph::common::local_conf();
        if (conf->ms_crc_data) {
          cluster_msgr.set_crc_data();
          client_msgr.set_crc_data();
        }
        if (conf->ms_crc_header) {
          cluster_msgr.set_crc_header();
          client_msgr.set_crc_header();
        }
        return seastar::do_with(ceph::mon::Client{conf->name,
                                                  client_msgr},
          (auto& monc) {
            
          }
                              )
    return test_monc().then([] {
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
