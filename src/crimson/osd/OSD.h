// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#include "crimson/net/Dispatcher.h"

class OSD : ceph::net::Dispatcher
{
  seastar::future<> ms_dispatch(ConnectionRef conn, MessageRef m) override;
};
