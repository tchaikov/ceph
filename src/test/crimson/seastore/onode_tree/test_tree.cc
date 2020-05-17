// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/crimson/gtest_seastar.h"

#include "crimson/common/log.h"
#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/segment_manager/ephemeral.h"
#include "crimson/os/seastore/onode_manager/onode_tree.h"

using namespace crimson;
using namespace crimson::os;
using namespace crimson::os::seastore;

namespace {
  [[maybe_unused]] seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_test);
  }
}

struct onode_tree_test_t : public seastar_test_suite_t {
  segment_manager::EphemeralSegmentManager
    segment_manager{segment_manager::DEFAULT_TEST_EPHEMERAL};
  Journal journal{segment_manager};
  Cache cache{segment_manager};
  LBAManagerRef
    lba_manager{lba_manager::create_lba_manager(segment_manager, cache)};
  TransactionManager tm{segment_manager, journal, cache, *lba_manager};

  onode_tree_test_t() = default;

  seastar::future<> set_up_fut() final {
    return segment_manager.init().safe_then([this] {
      return tm.mkfs();
    }).safe_then([this] {
      return tm.mount();
    }).handle_error(
      crimson::ct_error::all_same_way([] {
        ASSERT_FALSE("Unable to mount");
      })
    );
  }

  seastar::future<> tear_down_fut() final {
    return tm.close().handle_error(crimson::ct_error::all_same_way([] {
      ASSERT_FALSE("Unable to close");
    }));
  }
};

TEST_F(onode_tree_test_t, insert_single_leaf)
{
  run_async([this] {
    Btree btree{tm};
    ghobject_t oid{hobject_t{object_t{"saturn"}, "", 0, 0, 0, "solar"}};
    // add an onode
    auto onode = OnodeRef{new Onode{"hello"}};
    seastar::do_with(tm.create_transaction(),
      [&](auto& txn) {
      return btree.insert(oid, onode, *txn);
    }).unsafe_get();
      // read it back
    auto found = seastar::do_with(tm.create_transaction(),
      [&](auto& txn) {
      return btree.find(oid, *txn);
    }).unsafe_get0();
    ASSERT_TRUE(found);
    ASSERT_EQ(*onode, *found);
  });
}
