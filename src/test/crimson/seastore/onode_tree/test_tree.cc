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

// struct onode_tree_test_t : public seastar_test_suite_t {
//   segment_manager::EphemeralSegmentManager
//     segment_manager{segment_manager::DEFAULT_TEST_EPHEMERAL};
//   Cache cache{segment_manager};
//   paddr_t current{0, 0};

//   onode_tree_test_t() = default;

//   seastar::future<std::optional<paddr_t>>
//   submit_transaction(TransactionRef t)
//   {
//     auto record = cache.try_construct_record(*t);
//     if (!record) {
//       return seastar::make_ready_future<std::optional<paddr_t>>(
//         std::nullopt);
//     }

//     bufferlist bl;
//     for (auto &&block : record->extents) {
//       bl.append(block.bl);
//     }

//     ceph_assert((segment_off_t)bl.length() <
//                 segment_manager.get_segment_size());
//     if (current.offset + (segment_off_t)bl.length() >
//         segment_manager.get_segment_size())
//       current = paddr_t{current.segment + 1, 0};

//     auto prev = current;
//     current.offset += bl.length();
//     return segment_manager.segment_write(
//       prev,
//       std::move(bl),
//       true
//     ).safe_then(
//       [this, prev, t=std::move(t)] {
//         cache.complete_commit(*t, prev);
//         return seastar::make_ready_future<std::optional<paddr_t>>(prev);
//       },
//       crimson::ct_error::all_same_way([](auto e) {
//                                         ASSERT_FALSE("failed to submit");
//       })
//     );
//   }

//   auto get_transaction() {
//     return TransactionRef(new Transaction);
//   }

//   seastar::future<> set_up_fut() final {
//     return segment_manager.init().safe_then([this] {
//       return seastar::do_with(TransactionRef(new Transaction()),
//         [this](auto &transaction) {
// 	      return cache.mkfs(*transaction).safe_then([this, &transaction] {
// 	        return submit_transaction(std::move(transaction)).then([](auto p) {
//               ASSERT_TRUE(p);
// 	        });
// 	      });
// 	    });
//     }).handle_error(crimson::ct_error::all_same_way([](auto e) {
//       ASSERT_FALSE("failed to submit");
//     }));
//   }

//   seastar::future<> tear_down_fut() final {
//     return seastar::now();
//   }
// };

// TEST_F(onode_tree_test_t, insert_single_leaf)
// {
//   run_async([this] {
//     Btree btree;
//     {
//       // add an onode
//       auto t = get_transaction();
//       btree.insert(oid, onode, txn);
//       auto ret = submit_transaction(std::move(t)).get0();
//       ASSERT_TRUE(ret);
//     }
//     {
//       // read it back
//       auto t = get_transaction();
//       btree.find(oid).then([](OnodeRef found) {
//         ASSERT_TRUE(found);
//       });
//     }
//   });
// }
