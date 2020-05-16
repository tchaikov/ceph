// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include "btree.h"

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

void Btree::insert(const ghobject_t& oid, OnodeRef onode_ref,
                   TransactionRef txn)
{
  seastar::temporary_buffer<char> buffer{onode_ref->size()};
  onode_ref->encode(buffer.get_write(), buffer.size());
  auto onode = reinterpret_cast<const onode_t*>(buffer.get());

  return std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      auto extent = tm.alloc_extent<OnodeBlock>(*txn,
                                                L_ADDR_MIN, /* hint */
                                                BLOCK_SIZE);
      std::tie(root_addr, root) = LeafNode<BLOCK_SIZE, 0>::create(extent);
      [[maybe_unused]] auto promoted =
        std::get<1>(root).insert(oid, *onode, 0);
      assert(!promoted);
    } else {
      auto maybe_split = r.insert(oid, *onode, 0);
      if (maybe_split) {
        assert(maybe_split.change == maybe_split.created);
        // split at root node, let's create a new root indexing these nodes
        auto left_node = std::move(r);
        laddr_t left_addr = root_addr;
        std::tie(root_addr, root) = InnerNode<BLOCK_SIZE, 0>::create();
        [[maybe_unused]] auto promoted_left =
          std::get<2>(root).insert_at(0, left_node.first_oid(), left_addr, 0);
        assert(!promoted_left);
        [[maybe_unused]] auto promoted_right =
          std::get<2>(root).insert_at(1, maybe_split.oid, maybe_split.addr, 0);
        assert(!promoted_right);
      }
    }
  }, root);
}

void Btree::remove(const ghobject_t& oid)
{
  std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      return;
    } else {
      // root node does not have a parent, so my location is 0
      InnerNode<BLOCK_SIZE, 0>* dne = nullptr;
      [[maybe_unused]] update_t update = r.remove(oid, dne, 0U);
      // root node does not have siblings, and it does not remove itself
      assert(update.change == update.change_t::none);
    }
  }, root);
}

OnodeRef Btree::find(const ghobject_t& oid) const
{
  auto onode = std::visit([&](auto&& r) -> const onode_t* {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      return nullptr;
    } else {
      return r.find(oid);
    }
  }, root);
  if (onode) {
    return onode->decode();
  } else {
    return nullptr;
  }
}

void Btree::dump(std::ostream& os) const
{
  std::visit([&](auto&& r) {
    using T = std::decay_t<decltype(r)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      os << "[]";
    } else {
      r.dump(os, root_addr);
    }
  }, root);
}
