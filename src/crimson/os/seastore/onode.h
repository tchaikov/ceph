// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iostream>
#include <limits>

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

namespace crimson::os::seastore {

// in-memory onode, in addition to the stuff that should be persisted to disk,
// it may contain intrusive hooks for LRU, rw locks etc
class Onode : public boost::intrusive_ref_counter<
  Onode,
  boost::thread_unsafe_counter>
{
public:
  Onode(std::string_view s)
    : payload{s}
  {}
  const size_t size() const {
    return sizeof(uint16_t) + payload.size();
  }
  void* encode(void* buffer, size_t len) {
    struct encoded_t {
      uint16_t len;
      char data[];
    };
    auto p = reinterpret_cast<encoded_t*>(buffer);
    assert(std::numeric_limits<uint16_t>::max() >= size());
    assert(len >= size());
    p->len = payload.size();
    std::memcpy(p->data, payload.data(), payload.size());
    return static_cast<char*>(buffer) + payload.size();
  }
  friend std::ostream& operator<<(std::ostream &out, const Onode &rhs) {
    return out << rhs.get();
  }
  const std::string& get() const {
    return payload;
  }
  void encode(ceph::buffer::list& bl) const {
    using ceph::encode;
    uint8_t struct_v = 1;
    encode(struct_v, bl);
    uint16_t len = payload.size();
    encode(len, bl);
    bl.append(payload.data(), payload.size());
  }
  void decode(ceph::buffer::list::const_iterator& p) {
    using ceph::decode;
    uint8_t struct_v;
    decode(struct_v, p);
    uint16_t len;
    decode(len, p);
    payload = std::string(len, ' ');
    p.copy(len, payload.data());
  }

private:
  // dummy payload
  std::string payload;
};

WRITE_CLASS_ENCODER(Onode)

bool operator==(const Onode& lhs, const Onode& rhs) {
  return lhs.get() == rhs.get();
}

  using OnodeRef = boost::intrusive_ptr<Onode>;

}
