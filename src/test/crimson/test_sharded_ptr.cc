#include "crimson/thread/SharedPtr.h"
#include <chrono>
#include <map>
#include <random>
#include <core/app-template.hh>
#include <core/reactor.hh>
#include <core/sleep.hh>

using namespace std::chrono_literals;
using ceph::thread::Proxy;
using ceph::thread::ShardedPtr;

struct OSDMap {
  int data = 0;
};

class CacheService {
  static constexpr unsigned owner = 0;
  std::map<int, ShardedPtr<OSDMap>> cache;
  seastar::future<> put_map(int epoch, OSDMap* map) {
    return seastar::smp::submit_to(owner,
      [this, epoch, map, from=seastar::engine().cpu_id()] {
        cache[epoch].put(from, map);
      });
  }
public:
  seastar::future<Proxy<OSDMap>> add_map(int epoch, OSDMap&& map) {
    return seastar::smp::submit_to(owner, [this, epoch,
                                           from=seastar::engine().cpu_id(),
                                           map = std::move(map)] {
        cache[epoch] = ShardedPtr(seastar::make_lw_shared<OSDMap>(std::move(map)));
        OSDMap* map = cache[epoch].get(from);
        std::function<void(OSDMap*)> deleter = [this, from](OSDMap* map) {
          put_map(from, map);
        };
        return seastar::make_ready_future<ceph::thread::Proxy<OSDMap>>(
          Proxy{map, std::move(deleter)});
      });
  }
  seastar::future<Proxy<OSDMap>> get_map(int epoch) {
    return seastar::smp::submit_to(owner, [this, epoch,
                                           from=seastar::engine().cpu_id()] {
      if (auto found = cache.find(epoch); found != cache.end()) {
        OSDMap* map = found->second.get(from);
        std::function<void(OSDMap*)> deleter = [this, from](OSDMap* map) {
          put_map(from, map);
        };
        return seastar::make_ready_future<ceph::thread::Proxy<OSDMap>>(
          Proxy{map, std::move(deleter)});
      } else {
        throw std::runtime_error("map not found");
      }
    });
  }
};

static seastar::future<> test_sharded_ptr()
{
  return seastar::do_with(CacheService{},
    [](auto& cache) {
      int epoch = seastar::engine().cpu_id();
      return cache.add_map(epoch,
                           OSDMap{epoch * 2})
        .then([](auto added_map) {
          // wait until all maps are in place
          return seastar::sleep(5s);
        }).then([&cache] {
          auto gen = std::mt19937{std::default_random_engine()()};
          std::uniform_int_distribution<> dis{0,
              static_cast<int>(seastar::smp::count)};
          unsigned epoch = dis(gen);
          return cache.get_map(epoch);
        }).then([](Proxy<OSDMap> map) {
          auto m1 = map.shared_from_this();
          auto m2 = map.shared_from_this();
          auto m3 = m1;
          return seastar::now();
        });
    });
}

int main(int argc, char** argv)
{
  CacheService service;
  seastar::app_template app;
  return app.run(argc, argv, [] {
    return test_sharded_ptr().then([] {
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
 * unittest_seastar_sharded_ptr"
 * End:
 */
