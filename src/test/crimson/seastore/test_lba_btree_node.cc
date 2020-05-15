// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/crimson/gtest_seastar.h"

#include "crimson/common/log.h"

#include "crimson/os/seastore/lba_manager/btree/lba_btree_node_impl.h"

namespace {
  [[maybe_unused]] seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_test);
  }
}

using namespace crimson;
using namespace crimson::os;
using namespace crimson::os::seastore;
using namespace crimson::os::seastore::lba_manager;
using namespace crimson::os::seastore::lba_manager::btree;

template <typename T>
TCachedExtentRef<T> make_extent() {
  auto bp = ceph::bufferptr(LBA_BLOCK_SIZE);
  bp.zero();
  return CachedExtent::make_cached_extent_ref<T>(std::move(bp));
}

TEST(btree_node, basic) {
}

