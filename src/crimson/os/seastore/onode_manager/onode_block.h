// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <cstdint>

#include "crimson/os/seastore/transaction_manager.h"
#include "onode_delta.h"

namespace crimson::os::seastore {

// TODO s/CachedExtent/LogicalCachedExtent/
struct OnodeBlock final : LogicalCachedExtent {
  constexpr static segment_off_t SIZE = 4<<10;
  using Ref = TCachedExtentRef<OnodeBlock>;

  template <typename... T>
  OnodeBlock(T&&... t) : LogicalCachedExtent(std::forward<T>(t)...) {}

  OnodeBlock(OnodeBlock&& block) noexcept
    : LogicalCachedExtent{std::move(block)},
      delta{std::move(block.delta)}
  {}

  CachedExtentRef duplicate_for_write() final {
    return this;
  };

  // could materialize the pending changes to the underlying buffer here,
  // but since we write the change to the buffer immediately, let skip
  // this for now.
  void prepare_write() final {}

  // queries
  static constexpr extent_types_t TYPE = extent_types_t::ONODE_BLOCK;
  extent_types_t get_type() const final {
    return TYPE;
  }

  // have to stash all the changes before on_delta_write() is called,
  // otherwise we could pollute the extent with pending mutations
  // before the transaction carrying these mutations is committed to
  // disk
  ceph::bufferlist get_delta() final {
    bufferlist bl;
    delta.encode(bl);
    return bl;
  }

  void on_delta_write(paddr_t record_block_offset) final;
  void apply_delta(paddr_t base, ceph::bufferlist &bl) final;

private:
  template<class Node>
  void apply_delta_to_node(Node& node, delta_t&& delta);

  delta_t delta;
};

}

