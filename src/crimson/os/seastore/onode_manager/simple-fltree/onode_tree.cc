// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include "onode_tree.h"

#include <seastar/core/temporary_buffer.hh>

template<class Node, size_t NumThreshold>
class SplitByLikeness
{
public:
  std::pair<uint16_t, bool> split(const Node& node, uint16_t slot)
  {
    auto left_index = scan<1>(node, 0, node.count);
    auto right_index = scan<-1>(node, node.count - 1, -1);
    if (slot < left_index) {
      left_index += 1;
    } else if (slot >= right_index) {
      right_index -= 1;
    }
    if (auto right_num = node.count - right_index; left_index < right_num) {
      if (right_num >= NumThreshold) {
        return {right_index, true};
      }
    } else if (left_index >= NumThreshold) {
      return {left_index, true};
    } else {
      return {0, false};
    }
  }

private:
  // scan the elements in given node to split the largest consecutive group
  // into a separate node
  template<int step>
  uint16_t scan(const Node& node, int first, int last)
  {
    int index = 0;
    auto& [x_1, x_2] = node.key_at(first);
    for (index += step; index != last; index += step) {
      // the likeness relation between keys is transitive
      switch (auto& [y_1, y_2] = node.key_at(index); x_1.likes(y_1)) {
      case likes_t::yes:
        break;
      case likes_t::no:
        return index;
      case likes_t::maybe:
        if (x_2.likes(y_2)) {
          break;
        } else {
          return index;
        }
      }
    }
    return index;
  }
};

Btree::insert_ertr::future<>
Btree::insert(const ghobject_t& oid, OnodeRef onode,
              Transaction& txn)
{
  static constexpr InnerNode<BlockSize, 0>* dne{nullptr};

  return std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      return LeafNode<BlockSize, 0>::create(tm, txn).safe_then(
        [oid=oid, onode=std::move(onode), &txn, this](auto&& new_root) {
          root = std::move(new_root);
          return std::get<1>(root).insert(oid, onode, dne, 0, txn).safe_then([this](auto&& promoted) {
            assert(!promoted);
            return insert_ertr::make_ready_future<>();
        });
      });
    } else {
      return r.insert(oid, std::move(onode), dne, 0, txn).safe_then([&r, &txn, this](auto&& maybe_split) {
        if (!maybe_split) {
          return insert_ertr::make_ready_future();
        }
        assert(maybe_split.change == maybe_split.created);
        // split at root node, let's create a new root indexing these nodes
        return InnerNode<BlockSize, 0>::create(tm, txn).safe_then(
          [left_node=std::move(r), left_addr=root_addr,
           split_oid=maybe_split.oid, split_child=maybe_split.addr,
           &txn, this](auto&& new_root) {
          return new_root.insert_at(0, left_node.first_oid(), left_addr,
                                    dne, 0, txn).safe_then(
            [split_oid, split_child, &new_root, &txn, this](auto&& promoted_left) {
            assert(!promoted_left);
            return new_root.insert_at(1, split_oid, split_child, dne, 0, txn).safe_then(
              [&new_root, this](auto&& promoted_right) {
              assert(!promoted_right);
              this->root = std::move(new_root);
              return insert_ertr::make_ready_future();
            });
          });
        });
      });
    }
  }, root);
}

Btree::remove_ret Btree::remove(const ghobject_t& oid,
                                Transaction& txn)
{
  return std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      return remove_ertr::make_ready_future<>();
    } else {
      return remove_ertr::make_ready_future<>();
      // // root node does not have a parent, so my location is 0
      // InnerNode<BlockSize, 0>* dne = nullptr;
      // return r.remove(oid, dne, 0U, txn).safe_then([](update_t&& update) {
      //   // root node does not have siblings, and it does not remove itself
      //   assert(update.change == update.change_t::none);
      //   return remove_ertr::make_ready_future<>();
      // });
    }
  }, root);
}

Btree::find_ret
Btree::find(const ghobject_t& oid, Transaction& txn) const
{
  return std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      return find_ertr::make_ready_future<OnodeRef>();
    } else {
      return r.find(oid, txn);
    }
  }, root);
}

Btree::dump_ret Btree::dump(std::ostream& os, Transaction& txn) const
{
  return std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      os << "[]";
      return dump_ertr::make_ready_future<>();
    } else {
      return r.dump(os, root_addr, txn);
    }
  }, root);
}
