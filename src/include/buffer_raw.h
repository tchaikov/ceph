// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 20127 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_BUFFER_RAW_H
#define CEPH_BUFFER_RAW_H

#include <atomic>
#include <map>
#include "include/buffer.h"
#include "include/mempool.h"
#include "include/spinlock.h"

namespace ceph {
namespace buffer {

class raw {
public:
  char *data;
  unsigned len;
  std::atomic<unsigned> nref {0};
  int mempool;

  mutable ceph::spinlock crc_spinlock;
  std::map<std::pair<size_t, size_t>, std::pair<uint32_t, uint32_t> > crc_map;

  explicit raw(unsigned l, int mempool=mempool::mempool_buffer_anon)
    : data(NULL), len(l), nref(0), mempool(mempool) {
    mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
  }
  raw(char *c, unsigned l, int mempool=mempool::mempool_buffer_anon)
    : data(c), len(l), nref(0), mempool(mempool) {
    mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
  }
  virtual ~raw() {
    mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
      -1, -(int)len);
  }

  void _set_len(unsigned l) {
    mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
      -1, -(int)len);
    len = l;
    mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
  }

  void reassign_to_mempool(int pool) {
    if (pool == mempool) {
      return;
    }
    mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
      -1, -(int)len);
    mempool = pool;
    mempool::get_pool(mempool::pool_index_t(pool)).adjust_count(1, len);
  }

  void try_assign_to_mempool(int pool) {
    if (mempool == mempool::mempool_buffer_anon) {
      reassign_to_mempool(pool);
    }
  }

private:
  // no copying.
  // cppcheck-suppress noExplicitConstructor
  raw(const raw &other) = delete;
  const raw& operator=(const raw &other) = delete;
public:
  virtual char *get_data() {
    return data;
  }
  virtual raw* clone_empty() = 0;
  raw *clone() {
    raw *c = clone_empty();
    memcpy(c->data, data, len);
    return c;
  }
  virtual bool can_zero_copy() const {
    return false;
  }
  virtual int zero_copy_to_fd(int fd, loff_t *offset) {
    return -ENOTSUP;
  }
  virtual bool is_page_aligned() {
    return ((long)data & ~CEPH_PAGE_MASK) == 0;
  }
  bool is_n_page_sized() {
    return (len & ~CEPH_PAGE_MASK) == 0;
  }
  virtual bool is_shareable() {
    // true if safe to reference/share the existing buffer copy
    // false if it is not safe to share the buffer, e.g., due to special
    // and/or registered memory that is scarce
    return true;
  }
  bool get_crc(const std::pair<size_t, size_t> &fromto,
               std::pair<uint32_t, uint32_t> *crc) const {
    std::lock_guard<decltype(crc_spinlock)> lg(crc_spinlock);
    auto i = crc_map.find(fromto);
    if (i == crc_map.end()) {
        return false;
    }
    *crc = i->second;
    return true;
  }
  void set_crc(const std::pair<size_t, size_t> &fromto,
               const std::pair<uint32_t, uint32_t> &crc) {
    std::lock_guard<decltype(crc_spinlock)> lg(crc_spinlock);
    crc_map[fromto] = crc;
  }
  void invalidate_crc() {
    std::lock_guard<decltype(crc_spinlock)> lg(crc_spinlock);
    if (crc_map.size() != 0) {
      crc_map.clear();
    }
  }
};

} // namespace buffer
} // namespace ceph

#endif // CEPH_BUFFER_RAW_H
