// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <seastar/core/future.hh>

/**
 * PGBackend
 *
 * PGBackend defines an interface for logic handling IO and
 * replication on RADOS objects.  The PGBackend implementation
 * is responsible for:
 *
 * 1) Handling client operations
 * 2) Handling object recovery
 * 3) Handling object access
 * 4) Handling scrub, deep-scrub, repair
 */
class PGBackend {
public:
  virtual ~PGBackend() {}
  virtual seastar::future<bool> handle_message(Ref<Message> m);
private:
  seastar::future<int> do_osd_op(Ref<MOSDOp> m);
};
