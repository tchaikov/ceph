// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Red Hat, Inc
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#pragma once

#include <core/future.hh>

#include "Fwd.h"

namespace ceph {
namespace net {

class Messenger {
 protected:
  entity_addr_t my_addr;

 public:
  virtual ~Messenger() {}

  const entity_addr_t& get_myaddr() const { return my_addr; }

  /// bind to the given address
  virtual void bind(const entity_addr_t& addr) = 0;

  /// start the messenger
  virtual seastar::future<> start(Dispatcher *dispatcher) = 0;

  /// establish a client connection and complete a handshake
  virtual seastar::future<ConnectionRef> connect(const entity_addr_t& addr,
                                                 const entity_addr_t& myaddr) = 0;

  /// stop listenening and wait for all connections to close. safe to destruct
  /// after this future becomes available
  virtual seastar::future<> shutdown() = 0;
};

} // namespace net
} // namespace ceph
