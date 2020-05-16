// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "onode_block.h"

namespace crimson::os::seastore {

OnodeBlock::on_delta_write(paddr_t)
{
  // journal submitted to disk, now update the memory
  std::visit([&](auto& node) {
    apply_delta_to_node(node, delta);
  });
}

void OnodeBlock::apply_delta(paddr_t, ceph::bufferlist &bl)
{
  decode(delta, bl);
  std::visit([&](auto& node) {
    return apply_delta_to_node(node, std::move(delta));
  });
}

template<class Node>
OnodeBlock::apply_delta_to_node(Node& node, delta_t&& delta)
{
  switch (delta.op) {
  case delta.op_t::insert_onode:
    return node.insert_at(delta.oid, delta.onode);
  case delta.op_t::update_onode:
    return node.insert_child(delta.n, delta.oid);
  case delta.op_t::update_key:
    return node.insert_child(delta.n, delta.oid);
  case delta.op_t::shift_left:
    return node.shift_left(delta.n);
  case delta.op_t::truncate:
    return node.truncate(delta.n);
  case delta.op_t::insert_front:
    return node.insert_front(std::move(delta.keys), std::move(delta.cells));
  case delta.op_t::insert_back:
    return node.insert_back(std::move(delta.keys), std::move(delta.cells));
  case delta.op_t::remove_at:
    return node.remove_at(delta.n);
  default:
    assert(0 == "unknown onode delta");
  }
}
}
