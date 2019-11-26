// -*- mode: c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>
#include <seastar/core/future.hh>
#include <seastar/core/posix.hh>

namespace crimson::os::zns {

// represents a sequential write zone
// normally, the size zone is 256 / 512 MiB
// it uses a less tolerant state machine than the one defined by ZBC/ZNS
class Zone {
public:
  Zone(seastar::file_desc& fd);
  // zone management
  void open();
  void close();
  // reset write pointer
  void reset();
  // closed => full
  void finish();
  /// zoned append
  /// @return start LBA of written data
  seastar::future<uint64_t> append(seastar::temporary_buffer<char>&& data);
  seastar::future<> write(seastar::temporary_buffer<char>&& data);

private:
  bool is_opened() const;

  uint64_t start; // the start sector
  uint64_t size;  // size in number of sectors
  uint64_t capacity;
  uint64_t write_pointer;	// position in sectors
  // be compatible with blk_zone_cond in blkzoned.h
  enum class state_t : uint8_t {
    not_wp = 0,
    empty,
    imp_open,
    exp_open,
    closed,
    read_only,
    full,
    offline
  } state;

  // ideally, we should use seastar::file, but it does not support
  // zns/zbc commands at this moment.
  seastar::file_desc& fd;
};

} // crimson::os::zns
