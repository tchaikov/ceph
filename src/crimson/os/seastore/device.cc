// -*- mode: c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "device.h"

#include <memory>

namespace crimson::os::zns {

Device::Device(seastar::file_desc&& fd)
  : fd{std::move(fd)}
{}

uint32_t Device::get_zone_size()
{
  uint32_t zone_size = 0;
  fd.ioctl(BLKGETZONESZ, &zone_size);
  return zone_size;
}

uint32_t Device::get_num_zones()
{
  uint32_t nr_zones = 0;
  fd.ioctl(BLKGETNRZONES, &nr_zones);
  return nr_zones;
}

std::vector<blk_zone> Device::report_zones(uint64_t start_sector, uint64_t n)
{
  blk_zone_report *report = nullptr;
  auto buf = std::make_unique<char[]>(sizeof(*report) +
                                      (n * sizeof(report->zones[0])));
  report = reinterpret_cast<blk_zone_report*>(buf.get());
  report->sector = start_sector;
  report->nr_zones = n;
  fd.ioctl(BLKREPORTZONE, report);
  return {report->zones, report->zones + report->nr_zones};
}

seastar::future<size_t> Device::read(uint64_t pos, char* buf, size_t len)
{
  // we need to use ioctl(fd) for sending ZNS commands, but seastar::file
  // does not offer wrapper for them at this moment
  return seastar::make_ready_future<size_t>(fd.pread(buf, len, pos));
}

}
