// -*- mode: c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "zone.h"

#include <linux/blkzoned.h>
#include <vector>
#include <seastar/core/posix.hh>

namespace crimson::os::zns {

class Device {
public:
  Device(seastar::file_desc&& fd);
  uint32_t get_zone_size();
  uint32_t get_num_zones();
  template<typename Func>
  void for_each_zone(Func&& func) {
    constexpr uint32_t batch_size = 1U << 12;
    uint64_t start_sector = 0;
    auto num_zones = get_num_zones();
    while (num_zones > 0) {
      auto zones = report_zones(start_sector, std::min(num_zones, batch_size));
      for (auto& zone : zones) {
	func(zone);
	start_sector = zone.start + zone.len;
      }
      num_zones -= std::size(zones);
    }
  }
  seastar::future<size_t> read(uint64_t pos, char* buffer, size_t len);

private:
  std::vector<blk_zone> report_zones(uint64_t start, uint64_t n);
  seastar::file_desc fd;
};

} // crimson::os::zns

