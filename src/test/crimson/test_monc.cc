#include <seastar/core/app-template.hh>
#include "common/ceph_argparse.h"
#include "crimson/common/config_proxy.h"
#include "crimson/mon/MonClient.h"
#include "crimson/net/Connection.h"
#include "crimson/net/SocketMessenger.h"

using Config = ceph::common::ConfigProxy;
using MonClient = ceph::mon::Client;

template <typename T, typename... Args>
seastar::future<std::reference_wrapper<T>> create_sharded(Args... args) {
  auto sharded_obj = seastar::make_lw_shared<seastar::sharded<T>>();
  return sharded_obj->start(args...).then([sharded_obj]() {
    auto& ret = sharded_obj->local();
    seastar::engine().at_exit([sharded_obj]() {
      return sharded_obj->stop().finally([sharded_obj] {});
    });
    return std::ref(ret);
  });
}

static seastar::future<> test_monc()
{
  return ceph::common::sharded_conf().start(EntityName{}, string_view{"ceph"}).then([] {
    std::vector<const char*> args;
    std::string cluster;
    std::string conf_file_list;
    auto init_params = ceph_argparse_early_args(args,
                                                CEPH_ENTITY_TYPE_CLIENT,
                                                &cluster,
                                                &conf_file_list);
    auto& conf = ceph::common::local_conf();
    conf->name = init_params.name;
    conf->cluster = cluster;
    return conf.parse_config_files(conf_file_list);
  }).then([] {
    return ceph::common::sharded_perf_coll().start();
  }).then([] {
    // auto&& msgr_fut =
    //   create_sharded<ceph::net::SocketMessenger>(entity_name_t::OSD(0), "monc", 0);
    // return msgr_fut.then([](ceph::net::Messenger& msgr) {
    return create_sharded<ceph::net::SocketMessenger>(entity_name_t::OSD(0), "monc", 0).then([] (ceph::net::Messenger& msgr) {
      auto& conf = ceph::common::local_conf();
      if (conf->ms_crc_data) {
        msgr.set_crc_data();
      }
      if (conf->ms_crc_header) {
        msgr.set_crc_header();
      }
      return seastar::do_with(MonClient{msgr},
                              [&msgr](auto& monc) {
        return msgr.start(&monc).then([&monc] {
          return seastar::with_timeout(
            seastar::lowres_clock::now() + std::chrono::seconds{10},
            monc.start());
        }).then([&monc] {
          return monc.stop();
        });
      });
    });
  }).finally([] {
    return ceph::common::sharded_perf_coll().stop().then([] {
      return ceph::common::sharded_conf().stop();
    });
  });
}

int main(int argc, char** argv)
{
  seastar::app_template app;
  return app.run(argc, argv, [&] {
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
 * unittest_seastar_monc"
 * End:
 */
