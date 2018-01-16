// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <core/temporary_buffer.hh>
#include "include/buffer.h"


class seastar_buffer_iterator : details::buffer_iterator_impl<false> {
  using parent = details::buffer_iterator_impl<false>;
  using temporary_buffer = seastar::temporary_buffer<char>;
public:
  seastar_buffer_iterator(temporary_buffer& b)
    : parent(b.get_write(), b.end()), buf(b)
  {}
  using parent::pointer;
  using parent::get_pos_add;
  using parent::get;
  ceph::buffer::ptr get_ptr(size_t len);

private:
  // keep the reference to buf around, so it can be "shared" by get_ptr()
  temporary_buffer& buf;
};

class const_seastar_buffer_iterator : details::buffer_iterator_impl<true> {
  using parent = details::buffer_iterator_impl<true>;
  using temporary_buffer = seastar::temporary_buffer<char>;
public:
  const_seastar_buffer_iterator(temporary_buffer& b)
    : parent(b.get_write(), b.end())
  {}
  using parent::pointer;
  using parent::get_pos_add;
  using parent::get;
  ceph::buffer::ptr get_ptr(size_t len);
};
