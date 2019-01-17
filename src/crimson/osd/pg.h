// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <seastar/core/future.hh>

#include "osd/osd_types.h"
#include "crimson/net/Fwd.h"

class PGBackend;

namespace ceph::net {
  class Messenger;
}

namespace ceph::os {
  struct Collection;
  class CyanStore;
}

class OSD;

template<typename T> using Ref = boost::intrusive_ptr<T>;

class PG : public boost::intrusive_ref_counter<
  PG,
  boost::thread_unsafe_counter>
{
  OSD* osd_service;
  using ec_profile_t = std::map<std::string,std::string>;
public:
  PG(pg_pool_t&& pool, std::string&& name, ec_profile_t&& ec_profile);
  seastar::future<> handle_message(Ref<Message> m);
  static seastar::future<Ref<PG>> load(OSD* osd,
				       ceph::os::CyanStore* store,
				       spg_t pgid);

private:
  std::unique_ptr<PGBackend> backend;
  Ref<ceph::os::Collection> coll;
};
