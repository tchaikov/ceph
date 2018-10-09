#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>

#include "crimson/net/Dispatcher.h"

class OSD : public ceph::net::Dispatcher {
  seastar::gate gate;
public:
  OSD() = default;
  seastar::future<> start();
  seastar::future<> stop();
};
