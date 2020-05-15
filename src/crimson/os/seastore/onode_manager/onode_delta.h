// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cstdint>

#include "common/hobject.h"
#include "include/buffer_fwd.h"

#include "crimson/os/seastore/onode.h"
#include "crimson/os/seastore/seastore_types.h"

using crimson::os::seastore::OnodeRef;

enum class op_t : uint8_t {
  nop,
  insert_onode,
  update_onode,
  insert_child,
  update_key,
  shift_left,
  truncate,
  insert_front,
  insert_back,
  remove_at,
  // finer grained op?
  //  - changing the embedded extent map of given oid
  //  - mutating the embedded xattrs of given oid
};

struct delta_t {
  op_t opcode = op_t::nop;

  unsigned n = 0;
  ghobject_t oid;
  OnodeRef onode;
  ceph::bufferptr keys;
  ceph::bufferptr cells;

  delta_t() = default;
  delta_t(op_t opcode)
    : opcode{opcode}
  {}

  static delta_t nop() {
    return delta_t{op_t::nop};
  }
  static delta_t insert_onode(const ghobject_t& oid, OnodeRef onode) {
    delta_t delta{op_t::insert_onode};
    delta.oid = oid;
    delta.onode = onode;
    return delta;
  }
  static delta_t update_onode(const ghobject_t& oid, OnodeRef onode) {
    delta_t delta{op_t::update_onode};
    delta.oid = oid;
    delta.onode = onode;
    return delta;
  }
  static delta_t insert_child(unsigned slot, const ghobject_t& oid) {
    delta_t delta{op_t::insert_child};
    delta.n = slot;
    delta.oid = oid;
    return delta;
  }
  static delta_t update_key(unsigned slot, const ghobject_t& oid) {
    delta_t delta{op_t::update_key};
    delta.n = slot;
    delta.oid = oid;
    return delta;
  }
  static delta_t shift_left(unsigned n) {
    delta_t delta{op_t::shift_left};
    delta.n = n;
    return delta;
  }
  static delta_t truncate(unsigned n) {
    delta_t delta{op_t::truncate};
    delta.n = n;
    return delta;
  }
  static delta_t insert_front(ceph::buffer::ptr keys,
                              ceph::buffer::ptr cells)  {
    delta_t delta{op_t::insert_front};
    delta.keys = std::move(keys);
    delta.cells = std::move(cells);
    return delta;
  }
  static delta_t insert_back(ceph::buffer::ptr keys,
                             ceph::buffer::ptr cells)  {
    delta_t delta{op_t::insert_back};
    delta.keys = std::move(keys);
    delta.cells = std::move(cells);
    return delta;
  }
  static delta_t remove_at(unsigned slot) {
    delta_t delta{op_t::remove_at};
    delta.n = slot;
    return delta;
  }

  void encode(ceph::bufferlist& bl) {
    using ceph::encode;
    switch (opcode) {
    case op_t::insert_onode:
      [[fallthrough]];
    case op_t::update_onode:
      encode(oid, bl);
      encode(*onode, bl);
      break;
    case op_t::insert_child:
      [[fallthrough]];
    case op_t::update_key:
      encode(n, bl);
      encode(oid, bl);
      break;
    case op_t::shift_left:
      encode(n, bl);
      break;
    case op_t::truncate:
      encode(n, bl);
      break;
    case op_t::insert_front:
      [[fallthrough]];
    case op_t::insert_back:
      encode(n, bl);
      encode(keys, bl);
      encode(cells, bl);
      break;
    case op_t::remove_at:
      encode(n, bl);
      break;
    default:
      assert(0 == "unknown onode op");
    }
  }
  void decode(ceph::bufferlist::const_iterator& p) {
    using ceph::decode;
    decode(opcode, p);
    switch (opcode) {
    case op_t::insert_onode:
      [[fallthrough]];
    case op_t::update_onode:
      decode(oid, p);
      decode(*onode, p);
      break;
    case op_t::insert_child:
      [[fallthrough]];
    case op_t::update_key:
      decode(n, p);
      decode(oid, p);
      break;
    case op_t::shift_left:
      decode(n, p);
      break;
    case op_t::truncate:
      decode(n, p);
      break;
    case op_t::insert_front:
      [[fallthrough]];
    case op_t::insert_back:
      decode(n, p);
      decode(keys, p);
      decode(cells, p);
      break;
    case op_t::remove_at:
      decode(n, p);
      break;
    default:
      assert(0 == "unknown onode op");
    }
  }
};
