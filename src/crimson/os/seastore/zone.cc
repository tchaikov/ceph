// -*- mode: c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "zone.h"

#include <linux/blkzoned.h>

namespace crimson::os::zns {

Zone::Zone(seastar::file_desc& fd)
 : fd{fd}
{}

#ifndef BLKOPENZONE
// see https://github.com/torvalds/linux/commit/e876df1fe0ad1b191284ee6ed2db7960bd322d00#diff-5e3abee2820891548fae9fbd46b0d143
#define BLKOPENZONE   134
#define BLKCLOSEZONE  135
#define BLKFINISHZONE 136
#endif
void Zone::open()
{
  assert(state == state_t::empty ||
         state == state_t::imp_open ||
         state == state_t::closed);
  blk_zone_range range{start, size};
  fd.ioctl(BLKOPENZONE, &range);
  state = state_t::exp_open;
}

void Zone::close()
{
  assert(is_opened());
  blk_zone_range range{start, size};
  fd.ioctl(BLKCLOSEZONE, &range);
  state = state_t::closed;
}

void Zone::reset()
{
  assert(state == state_t::full);
  blk_zone_range range{start, size};
  fd.ioctl(BLKRESETZONE, &range);
  write_pointer = 0;
  state = state_t::empty;
}

void Zone::finish()
{
  assert(state == state_t::closed);
  blk_zone_range range{start, size};
  fd.ioctl(BLKFINISHZONE, &range);
  state = state_t::full;  
}

seastar::future<uint64_t> Zone::append(seastar::temporary_buffer<char>&& data)
{
  if (state == state_t::empty) {
    // open implicitly
    state = state_t::imp_open;
  }
  assert(is_opened());
  return seastar::make_ready_future<uint64_t>(0);
}

seastar::future<> Zone::write(seastar::temporary_buffer<char>&& data)
{
  if (state == state_t::empty) {
    // open implicitly
    state = state_t::imp_open;
  }
  assert(is_opened());
  // should use async
  auto p = data.get();
  auto end = p + data.size();
  while (p < end) {
    auto r = fd.write(p, end - p);
    if (r) {
      p += *r;
    }
  }
  return seastar::make_ready_future<>();
}

bool Zone::is_opened() const
{
  return (state == state_t::imp_open ||
          state == state_t::exp_open);
}

}
