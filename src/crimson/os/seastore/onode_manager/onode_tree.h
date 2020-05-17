// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/range/irange.hpp>

#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/cached_extent.h"
#include "crimson/os/seastore/onode_manager/onode_block.h"
#include "crimson/os/seastore/onode_manager/onode_node.h"
#include "crimson/os/seastore/transaction_manager.h"

using crimson::os::seastore::OnodeBlock;
using crimson::os::seastore::TCachedExtentRef;
using crimson::os::seastore::TransactionManager;
using crimson::os::seastore::Transaction;
using crimson::os::seastore::L_ADDR_MIN;

// insertion/removal/resize
//
// - [   ] [updated]
// -       [       ] [updated]
// - [   ] [removed]
// -       [       ] [removed]
// -       [removed]
// - [new] [       ]
// -       [       ]
// -       [       ] [new]
// -       [       ] [updated]
struct [[nodiscard]] update_t {
  enum change_t {
    none,
    updated,
    removed,
    created,
  } change = none;
  unsigned slot = 0;
  ghobject_t oid;
  uint64_t addr;

  static update_t make_update(unsigned slot, ghobject_t&& oid) {
    return {updated, slot, std::move(oid)};
  }

  static update_t make_removed(unsigned slot) {
    return {removed, slot};
  }

  static update_t make_created(unsigned slot, ghobject_t&& oid, uint64_t addr) {
    return {created, slot, std::move(oid), addr};
  }
  update_t() = default;

  operator bool() const {
    return change != change_t::none;
  }
};

// forward declarations for friend declarations
template<size_t, int> class LeafNode;
template<size_t, int> class InnerNode;

// life cycle management
// merge/split/traverse in a node
template<size_t BlockSize,
         int N,
         ntype_t NodeType>
class BaseNode {
public:
  using my_node_t = node_t<BlockSize, N, NodeType>;
  using key1_t = typename my_node_t::key_t;
  using key2_t = typename my_node_t::partial_key_t;
  using const_item_t = typename my_node_t::const_item_t;

  uint16_t count() const {
    return me->count;
  }

  const_item_t item_at(unsigned slot) const {
    return me->item_at(me->keys[slot]);
  }
  // helpers to construct variant<Nodes...>
  template<int Level, int MaxLevel, class... Nodes>
  struct child_node_variant : child_node_variant<Level + 1,
                                                 MaxLevel,
                                                 InnerNode<BlockSize, Level>,
                                                 LeafNode<BlockSize, Level>,
                                                 Nodes...>
  {};
  template<int MaxLevel, class... Nodes>
  struct child_node_variant<MaxLevel, MaxLevel, Nodes...>
  {
    using type = std::variant<Nodes...>;
  };
  // [MinN, MaxN)
  template<int MinN, int MaxN> using child_node_variant_t =
    typename child_node_variant<std::max(0, MinN), std::min(MaxN, 4)>::type;

  template<int MinLevel, int Level, class... Nodes>
  struct parent_node_variant : parent_node_variant<MinLevel,
                                                   Level - 1,
                                                   InnerNode<BlockSize, Level>,
                                                   Nodes...>
  {};
  template<int MinLevel, class... Nodes>
  struct parent_node_variant<MinLevel, MinLevel, Nodes...>
  {
    using type = std::variant<std::monostate, Nodes...>;
  };
  // (-1, N]
  using parent_node_variant_t = typename parent_node_variant<-1, N>::type;

  // 3 cases of insertion
  //
  // 1. the target leaf is found!
  //    - LeafNode::insert_at(slot, partial_key, onode)
  //      we can remove the matched prefix from oid and build partial key used
  //      by the leaf, or just pass the oid
  // 2. a node split a sibling aside of it
  //    * if the node has a parent node:
  //      - parent.promote(ref_slot, left_or_right, partial_key, child_addr)
  //        slot = left_or_rigt == left ? ref_slot - 1 : ref_slot + 1
  //        ref_key = key_at(slot)
  //        partial_key = key_t{key_at(slot), partial_key};
  //    * if the node is the root
  //      1. create a new root by promotion
  //        - Node<0>::Node(root.first_key(), root.addr)
  //      2. the new root points to the old root and its sibling
  //        - Node<0>::insert(0, child.first_key(), child.addr)
  //          where child is also of Node<0>
  // insert an element
  // @return the changed boundary that the parent should be aware of
  seastar::future<update_t> _insert_at(unsigned slot,
				       const ghobject_t& oid,
				       const_item_t item,
				       unsigned whoami,
				       Transaction& txn) {
    if (me->is_overflow(oid, item)) {
      return split_with(oid, whoami, slot, txn,
	[&](auto& node, unsigned n) { node.insert_at(n, oid, item); });
    } else {
      me->insert_at(slot, oid, item);
      return seastar::make_ready_future<update_t>();
    }
  }

  template<class Item>
  seastar::future<update_t> insert_at(unsigned slot,
				      const ghobject_t& oid,
				      Item item,
				      unsigned whoami,
				      Transaction& txn)
  {
    if constexpr (std::is_same_v<Item, laddr_t>) {
      return _insert_at(slot, oid, item, whoami, txn);
    } else {
      seastar::temporary_buffer<char> buf{item->size()};
      item->encode(buf.get_write(), buf.size());
      auto onode = reinterpret_cast<const onode_t*>(buf.get());
      return _insert_at(slot, oid, *onode, whoami, txn);
    }
  }

  template<class DoWith>
  seastar::future<update_t>
  split_with(const ghobject_t& oid, unsigned whoami, unsigned slot,
	     Transaction& txn, DoWith&& do_with)
  {
    // TODO: SplitByLikeness
    //       SplitBySize
    return create(txn).safe_then([do_with=std::forward<DoWith>(do_with),
				  oid, whoami, slot, this](auto& new_node) {
      // move [split_at, count) to new_node
      uint16_t split_at = me->count / 2;
      new_node.me->move_from(*me, split_at, 0, me->count - split_at);
      if (slot <= split_at) {
        std::invoke(std::move(do_with), *me, slot);
      } else {
        std::invoke(std::move(do_with), *new_node.me, slot - split_at);
      }
      return update_t::make_created(whoami + 1,
				    new_node.me->update_oid_with_slot(0, oid),
				    new_node.addr());
    });
  }

  ghobject_t first_oid() const {
    return me->update_oid_with_slot(0, ghobject_t{});
  }

  template<int UpN>
  update_t grab_or_merge_left(const InnerNode<BlockSize, UpN>& parent,
                              unsigned whoami,
                              uint16_t min_grab, uint16_t max_grab,
			      Transaction& txn)
  {
    if (whoami == 0) {
      return {};
    }
    return parent.load_child(whoami - 1, txn).safe_then(
      [&parent, whoami, min_grab, max_grab, this](auto& v) {
      return std::visit([&](auto& left) -> update_t {
        auto [n, bytes] = left.get().calc_grab_back(min_grab, max_grab);
        if (n == 0) {
          return {};
        } else if (n == left.count()) {
          if (left.get().node_type != me->node_type) {
            assert(0);
            return {};
          }
          auto mover = make_mover(parent.get(), *me, left.get(), whoami);
          left.get().acquire_right(*me, whoami - 1, mover);
          return update_t::make_removed(whoami + 1);
        } else {
          auto mover = make_mover(parent.get(), left.get(), *me, whoami - 1);
          me->grab_from_left(left.get(), n, bytes, mover);
          return update_t::make_update(whoami, first_oid());
        }
      }, v);
    });
  }

  template<int UpN>
  update_t grab_or_merge_right(const InnerNode<BlockSize, UpN>& parent,
                               unsigned whoami,
                               uint16_t min_grab, uint16_t max_grab,
			       Transaction& txn)
  {
    if (whoami + 1 == parent.count()) {
      return {};
    }
    auto v = parent.load_child(whoami + 1);
    return std::visit([&](auto& right) -> update_t {
      auto [n, bytes] = right.get().calc_grab_front(min_grab, max_grab);
      if (n == 0) {
        return {};
      } else if (n == right.count()) {
        auto mover = make_mover(parent.get(), right.get(), *me, whoami + 1);
        me->acquire_right(right.get(), whoami, mover);
        right.destroy(txn);
        return {update_t::change_t::removed, whoami + 1, ghobject_t{}};
      } else {
        auto mover = make_mover(parent.get(), right.get(), *me, whoami + 1);
        me->grab_from_right(right.get(), n, bytes, mover);
        return {update_t::change_t::updated, whoami + 1, right.first_oid()};
      }
    }, v);
  }

  template<int UpN, class DoWith>
  update_t push_to_left(const InnerNode<BlockSize, UpN>& parent,
                        unsigned whoami,
                        uint16_t min_push,
                        uint16_t max_push,
                        unsigned slot,
                        DoWith&& do_with)
  {
    if (whoami == 0) {
      return {};
    }
    auto v = parent.load_child(whoami - 1);
    return std::visit([&](auto& left) -> update_t {
      auto [n, bytes] = me->calc_grab_front(min_push, max_push);
      if (n == 0) {
        return {};
      } else {
        assert(n <= me->count);
        auto mover = make_mover(parent.get(), *me, left, whoami);
        me->push_to_left(left.get(), n, bytes, mover);
        if (slot < n) {
          std::invoke(std::forward<DoWith>(do_with),
                      left.get(), left.count() - (n - slot));
        } else {
          std::invoke(std::forward<DoWith>(do_with),
                      *me, slot - n);
        }
        return {update_t::change_t::updated, whoami, first_oid()};
      }
    }, v);
  }

  template<int UpN, class DoWith>
  update_t push_to_right(const InnerNode<BlockSize, UpN>& parent,
                         unsigned whoami,
                         uint16_t min_push,
                         uint16_t max_push,
                         unsigned slot,
                         DoWith&& do_with)
  {
    if (whoami + 1 == parent.count()) {
      return {};
    }
    auto v = parent.load_child(whoami + 1);
    return std::visit([&](auto& right) -> update_t {
      auto [n, bytes] = me->calc_grab_back(min_push, max_push);
      if (n == 0) {
        return {};
      } else {
        assert(n <= me->count);
        auto mover = make_mover(parent.get(), *me, right, whoami);
        me->push_to_right(right.get(), n, bytes, mover);
        if (slot > me->count) {
          std::invoke(std::forward<DoWith>(do_with), right.get(), slot - me->count);
        } else {
          std::invoke(std::forward<DoWith>(do_with), *me, slot);
        }
        return {update_t::change_t::updated, whoami + 1, right.first_oid()};
      }
    }, v);
  }

  using remove_ertr = TransactionManager::read_extent_ertr;
  using remove_ret = remove_ertr::future<update_t>;
  template<int UpN>
  remove_ret remove_from(unsigned slot,
			 InnerNode<BlockSize, UpN>* parent,
			 unsigned whoami,
			 Transaction& txn)
  {
    me->remove_from(slot);
    if (!parent) {
      // we can live with an empty root node
      return remove_ertr::make_ready_future<update_t>();
    }
    if (count() == 0) {
      return remove_ertr::make_ready_future<update_t>(update_t::make_removed(whoami));
    } else if (me->is_underflow(me->used_space())) {
      const auto [min_grab, max_grab] = me->bytes_to_grab(me->used_space());
      // rebalance
      if (auto result = grab_or_merge_left(*parent, whoami,
					   min_grab, max_grab); result) {
        return remove_ertr::make_ready_future<update_t>(result);
      } else if (auto result = grab_or_merge_right(*parent, whoami,
						   min_grab, max_grab); result) {
        return remove_ertr::make_ready_future<update_t>(result);
      } else {
        // yes, we leave this node underfull, because if we steal elements
        // from its siblings could lead to less optimum distribution of
        // indexed entries
	return remove_ertr::make_ready_future<update_t>();
      }
    } else {
      return remove_ertr::make_ready_future<update_t>();
    }
  }

  using create_ertr = TransactionManager::alloc_extent_ertr;
  using create_ret = create_ertr::future<BaseNode>;
  create_ret create(Transaction& txn) {
    return tm->alloc_extent<OnodeBlock>(txn, L_ADDR_MIN, BlockSize).safe_then(
      [this](auto&& extent) {
      return create_ertr::make_ready_future<BaseNode>(&tm, std::move(extent));
    });
  }

  using read_block_ertr = TransactionManager::read_extent_ertr;
  using read_block_ret = read_block_ertr::future<OnodeBlock::Ref>;
  read_block_ret load_block(unsigned slot, Transaction& txn) const {
    laddr_t addr = this->item_at(slot);
    return tm->read_extents<OnodeBlock>(txn, addr, BlockSize).safe_then(
      [](auto&& extents) {
      assert(extents.size() == 1);
      [[maybe_unused]] auto [laddr, e] = extents.front();
      return read_block_ertr::make_ready_future<OnodeBlock::Ref>(std::move(e));
    });
  }

  laddr_t addr() const {
    return extent->get_laddr();
  }

  void destroy(Transaction& txn) {
    txn.add_to_retired_set(extent);
  }

  BaseNode(TransactionManager* tm, OnodeBlock::Ref&& extent)
    : tm{tm},
      extent{std::move(extent)},
      me{reinterpret_cast<my_node_t*>(this->extent->get_bptr().c_str())}
  {}

  my_node_t& get() {
    return *me;
  }

  const my_node_t& get() const {
    return *me;
  }

  BaseNode(BaseNode&& node) noexcept
    : tm{node.tm},
      extent{std::move(extent)},
      me{me}
  {
    node.tm = nullptr;
    node.extent.reset();
    node.me = nullptr;
  }
  BaseNode& operator=(BaseNode&& node) noexcept {
    assert(!tm);
    assert(!extent);
    assert(!me);

    std::swap(tm, node.tm);
    std::swap(extent, node.extent);
    std::swap(me, node.me);
  }
  friend class LeafNode<BlockSize, N>;
  friend class InnerNode<BlockSize, N>;

  // otherwise we need to pass tm to all calls which might
  // - load a node, as it reads an extent: inner node does this if the call
  //     needs to descend down to a leaf node, and it always does.
  // - create a node, as it allocates a new extent
  //     if the mutation increases the size of a node and potentially causes
  //     node split
  // store a pointer so we can move
  TransactionManager* tm = nullptr;
  TCachedExtentRef<OnodeBlock> extent;
  my_node_t* me;
};

template<size_t BlockSize, int N>
class LeafNode : private BaseNode<BlockSize, N, ntype_t::leaf> {
public:
  using base_t = BaseNode<BlockSize, N, ntype_t::leaf>;
  using typename base_t::my_node_t;
  using base_t::count;
  using base_t::get;
  using base_t::insert_at;
  using base_t::remove_from;
  using base_t::first_oid;

  using find_ertr = TransactionManager::read_extent_ertr;
  using find_ret = find_ertr::future<OnodeRef>;
  find_ret find(const ghobject_t& oid, Transaction&) const {
    auto [slot, found] = this->me->lower_bound(oid);
    if (found) {
      return find_ertr::make_ready_future<OnodeRef>(this->item_at(slot).decode());
    } else {
      return find_ertr::make_ready_future<OnodeRef>();
    }
  }

  seastar::future<update_t> insert(const ghobject_t& oid,
				   OnodeRef onode,
				   unsigned whoami,
				   Transaction& txn)
  {
    auto [slot, found] = this->me->lower_bound(oid);
    assert(!found);
    return insert_at(slot, oid, onode, whoami, txn);
  }

  using remove_ertr = TransactionManager::read_extent_ertr;
  using remove_ret = remove_ertr::future<update_t>;
  template<int UpN>
  remove_ret remove(const ghobject_t& oid,
		    InnerNode<BlockSize, UpN>* parent,
		    unsigned whoami,
		    Transaction& txn)
  {
    auto [slot, found] = this->me->lower_bound(oid);
    if (found) {
      return base_t::remove_from(slot, parent, whoami, txn);
    } else {
      return remove_ertr::make_ready_future<update_t>();
    }
  }

  using dump_ertr = TransactionManager::read_extent_ertr;
  using dump_ret = find_ertr::future<>;
  dump_ret dump(std::ostream& os, laddr_t addr, Transaction&) const {
    os << "Node<" << N << ", leaf> "
       << "@ " << std::hex << addr << std::dec << " "
       << std::setprecision(4)
       << (float(this->me->used_space()) / this->me->capacity())
       << std::setprecision(6) << "\n";
    this->me->dump(os);
    return dump_ertr::make_ready_future<>();
  }

  using create_ertr = TransactionManager::alloc_extent_ertr;
  static create_ertr::future<LeafNode>
  create(TransactionManager& tm, Transaction& txn) {
    static_assert(N == 0);
    return tm.alloc_extent<OnodeBlock>(txn, L_ADDR_MIN, BlockSize).safe_then([&tm](auto&& extent) {
      return create_ertr::make_ready_future<LeafNode>(&tm, extent);
    });
  }
  // for InnerNode::load_child()
  LeafNode(TransactionManager* tm, OnodeBlock::Ref extent)
    : base_t{tm, std::move(extent)}
  {}

  LeafNode(LeafNode&& node) noexcept
    : base_t{std::move(node)}
  {}
  LeafNode& operator=(LeafNode&& node) {
    base_t::operator=(std::move(node));
    return *this;
  }
private:
  // for LeafNode::create()
  LeafNode(my_node_t* me)
    : base_t{me}
  {}
};

template<size_t BlockSize, int N>
class InnerNode : private BaseNode<BlockSize, N, ntype_t::inner> {
public:
  using base_t = BaseNode<BlockSize, N, ntype_t::inner>;
  using typename base_t::my_node_t;
  using base_t::count;
  using base_t::get;
  using base_t::insert_at;
  using base_t::first_oid;

  // i can have children of InnerNode<Lv>, N <= Lv < 4
  using child_node_t = typename base_t::child_node_variant_t<N, 4U>;

  std::pair<unsigned, bool> lower_bound(const ghobject_t& oid) const {
    auto [slot, found] = this->me->lower_bound(oid);
    if (found) {
      return {slot, true};
    } else {
      return {slot ? --slot : 0, false};
    }
  }

  using find_ertr = TransactionManager::read_extent_ertr;
  using find_ret = find_ertr::future<OnodeRef>;
  find_ret find(const ghobject_t& oid, Transaction& txn) const {
    [[maybe_unused]] auto [slot, found] = lower_bound(oid);
    return load_child(slot, txn).safe_then([oid, &txn](auto& v) {
      return std::visit([&](auto& child) {
        return child.find(oid, txn);
      }, v);
    });
  }

  using insert_ertr = TransactionManager::read_extent_ertr;
  using insert_ret = find_ertr::future<update_t>;
  // insert() in InnerNode is different than BaseNode::insert_at()
  seastar::future<update_t> insert(const ghobject_t& oid,
				   OnodeRef onode,
				   unsigned whoami,
				   Transaction& txn)
  {
    auto [slot, found] = lower_bound(oid);
    assert(!found);
    return load_child(slot, txn).then([=](auto&& v) {
      return std::visit([&](auto& child) -> update_t {
        return child.insert(oid, onode, slot, txn).then([whoami, this](auto&& maybe_promote) {
          if (maybe_promote) {
            return this->insert_at(maybe_promote.slot,
                                   maybe_promote.oid,
                                   maybe_promote.addr,
                                   whoami);
          } else {
            return insert_ertr::make_ready_future<>();
          }
       }, v);
     });
   });
  }

  // @param location the slot in which this inner node is in,
  //        0 if this is a root node.
  using remove_ertr = TransactionManager::read_extent_ertr;
  using remove_ret = remove_ertr::future<update_t>;
  template<int UpN>
  remove_ret remove(const ghobject_t& oid,
		    InnerNode<BlockSize, UpN>* parent,
		    unsigned whoami,
		    Transaction& txn) {
    [[maybe_unused]] auto [slot, found] = lower_bound(oid);
    return load_child(slot, txn).safe_then([slot, oid, whoami, parent, &txn, this](auto&& v) {
      return std::visit([&](auto& child) {
        // descend down to my child
	return child.remove(oid, this, slot, txn).safe_then(
          [parent, whoami, &txn, this](update_t& result) {
          // update my own slots, and propagate any intersting changes back to my
          // parent
          switch (result.change) {
          case result.change_t::updated:
            return update_key_at(result.slot, parent, whoami, result.oid);
          case result.change_t::removed:
            return base_t::remove_from(result.slot, parent, whoami, txn);
          case result.change_t::none:
            return remove_ertr::make_ready_future<update_t>();
          default:
            assert(0);
          }
        });
      }, v);
    });
  }

  // TODO: updating the key of a child addr could enlarge the node, or the
  //       other way around as the size variable-sized key of that item could
  //       increase / decrease. so instead of returning a "update_t", we
  //       should return a variant<update_t, update_t> or an
  //       equivalent of it
  template<int UpN>
  update_t update_key_at(unsigned slot,
                         InnerNode<BlockSize, UpN>* parent,
                         unsigned whoami,
                         const ghobject_t& oid) {
    switch (uint16_t size = this->me->size_with_key(slot, oid);
            this->me->size_state(size)) {
    case size_state_t::underflow:
      this->me->update_key_at(slot, oid);
      if (parent) {
        const auto [min_grab, max_grab] = this->me->bytes_to_grab(size);
        if (auto result = this->grab_or_merge_left(*parent, whoami, min_grab, max_grab)) {
          return result;
        } else if (auto result = this->grab_or_merge_right(*parent, whoami, min_grab, max_grab)) {
          return result;
        } else {
          // leave the node underfull
        }
      } else {
        return {};
      }
    case size_state_t::okay:
      this->me->update_key_at(slot, oid);
      return {};
    case size_state_t::overflow:
      if (parent) {
        const auto [min_push, max_push] = this->me->bytes_to_push(size);
        // TODO: take the new size at slot into consideration
        if (auto result = this->push_to_left(*parent, whoami, min_push, max_push, slot,
          [&](auto& node, unsigned n) { node.update_key_at(n, oid); })) {
          return result;
        } else if (auto result = this->push_to_right(*parent, whoami, min_push, max_push, slot,
          [&](auto& node, unsigned n) { node.update_key_at(slot, oid); })) {
          return result;
        } else {
          return this->split_with(oid, whoami, slot,
            [&](auto& node, unsigned n) { node.update_key_at(n, oid); });
        }
      } else {
        return this->split_with(oid, whoami, slot,
          [&](auto& node, unsigned n) { node.update_key_at(n, oid); });
      }
    default:
      assert(0);
    }
  }

  using dump_ertr = TransactionManager::read_extent_ertr;
  using dump_ret = find_ertr::future<>;
  dump_ret dump(std::ostream& os, laddr_t addr, Transaction& txn) const {
    os << "Node<" << N << ", inner> "
       << "@ " << std::hex << addr << std::dec << " "
       << std::setprecision(4)
       << (float(this->me->used_space()) / this->me->capacity())
       << std::setprecision(6) << "\n";
    this->me->dump(os);
    auto slots = boost::irange((uint16_t)0, this->me->count);
    return crimson::do_for_each(slots, [&](auto slot) {			 
      return load_child(slot, txn).safe_then([&os, &txn, slot, this](auto&& v) {
        return std::visit([&](auto& child) {
          return child.dump(os, this->item_at(slot), txn);
        }, v);
      });
    });
  }

  // helper to create a node
  template<int ThisN, int MaxN>
  static child_node_t load_node_at(int n, bool is_leaf,
				   TransactionManager* tm,
				   OnodeBlock::Ref extent)
  {
    static_assert(ThisN < MaxN);
    if (n == ThisN) {
      if (is_leaf) {
        return LeafNode<BlockSize, ThisN>(tm, std::move(extent));
      } else {
        return InnerNode<BlockSize, ThisN>(tm, std::move(extent));
      }
    } else if constexpr (ThisN + 1 < MaxN) {
      return load_node_at<ThisN + 1, MaxN>(n, is_leaf, tm, std::move(extent));
    } else {
      // bad tag
      throw std::logic_error(fmt::format("bad node level={}, leaf={}", n, is_leaf));
    }
  }

  using load_node_ertr = TransactionManager::read_extent_ertr;
  using load_node_ret = load_node_ertr::future<child_node_t>;
  load_node_ret load_child(unsigned slot, Transaction& txn) const {
    static_assert(N < 4);
    return this->load_block(slot, txn).safe_then([this](OnodeBlock::Ref block) {
    // the L of child nodes can only be L, L.., 3
      auto p = reinterpret_cast<uint8_t*>(block->get_bptr().c_str());
      tag_t tag{*p};
      return load_node_ertr::make_ready_future<child_node_t>(
	load_node_at<N, 4>(tag.layout(), tag.is_leaf(), this->tm, std::move(block)));
    });
  }

  using create_ertr = TransactionManager::alloc_extent_ertr;
  using create_ret = create_ertr::future<InnerNode>;
  static create_ret create(TransactionManager& tm, Transaction& txn) {
    static_assert(N == 0);
    return tm.alloc_extent<OnodeBlock>(txn, L_ADDR_MIN, BlockSize).safe_then([&tm](auto&& extent) {
      return create_ertr::make_ready_future<InnerNode>(&tm, std::move(extent));
    });
  }
  // for InnerNode::create()
  InnerNode(TransactionManager* tm, OnodeBlock::Ref&& extent)
    : base_t{tm, std::move(extent)}
  {}
  InnerNode(InnerNode&& node) noexcept
    : base_t{std::move(node)}
  {}
  InnerNode& operator=(InnerNode&& node) {
    base_t::operator=(std::move(node));
    return *this;
  }

private:
  // for InnerNode::load_child()
  InnerNode(my_node_t* me)
    : base_t{me}
  {}
};

template<size_t BlockSize>
class Cursor {
public:
  std::variant<LeafNode<BlockSize, 0>,
               LeafNode<BlockSize, 1>,
               LeafNode<BlockSize, 2>,
               LeafNode<BlockSize, 3>> node;
  unsigned slot;
  const onode_t& operator*() const {
    return std::visit([=](auto& n) {
      return n->item_at(slot);
    });
  }
};

class Btree
{
public:
  Btree(TransactionManager& tm)
    : tm{tm}
  {}
  using insert_ertr = TransactionManager::alloc_extent_ertr;
  insert_ertr::future<> insert(const ghobject_t& oid, OnodeRef onode, Transaction& txn);

  using remove_ertr = TransactionManager::read_extent_ertr;
  using remove_ret = remove_ertr::future<>;
  remove_ret remove(const ghobject_t& oid, Transaction& txn);

  using find_ertr = TransactionManager::read_extent_ertr;
  using find_ret = find_ertr::future<OnodeRef>;
  find_ret find(const ghobject_t& oid, Transaction& txn) const;

  using dump_ertr = TransactionManager::read_extent_ertr;
  using dump_ret = find_ertr::future<>;
  dump_ret dump(std::ostream& os, Transaction& txn) const;

private:
  template<class Node>
  std::optional<onode_t> locate_in(const Node& node,
                                   const ghobject_t& oid) const;
  template<class Node>
  typename Node::split_update_t insert_into(Node* node,
                                            const ghobject_t& oid,
                                            const onode_t& onode);
  // 16K is the default block size used by btrfs, let's start with 256
  static constexpr size_t BlockSize = 512;

  laddr_t root_addr = 0;
  std::variant<std::monostate,
               LeafNode<BlockSize, 0>,
               InnerNode<BlockSize, 0>> root;
private:
  TransactionManager& tm;
};
