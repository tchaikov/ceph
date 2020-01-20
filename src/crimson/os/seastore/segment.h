// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <seastar/core/future.hh>

namespace crimson::os {

class Segment {
  seastar::future<segment_off_t> write(Record&&);
  seastar::future<> close();
};

}
