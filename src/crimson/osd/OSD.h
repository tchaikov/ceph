// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/net/Dispatcher.h"
#include "crimson/net/Fwd.h"

#include "core/lowres_clock.hh"


enum class OSDState {
  INITIALIZING,
  PREBOOT,
  BOOTING,
  ACTIVE,
  STOPPING,
  WAITING_FOR_HEALTHY,
};

class OSD : public ceph::net::Dispatcher
{
  seastar::timer<seastar::lowres_clock> tick_timer;
  seastar::timer<seastar::lowres_clock> tick_timer_without_osd_lock;

  void tick();
  void tick_without_osd_lock();

  OSDState state {OSDState::INITIALIZING};

public:

public:
  OSD();
  void init();
  seastar::future<> ms_dispatch(ceph::net::ConnectionRef conn,
				MessageRef m) override;
};
