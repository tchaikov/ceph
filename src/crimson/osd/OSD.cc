// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab
#include "crimson/net/Dispatcher.h"
#include "OSD.h"

OSD::OSD()
  : tick_timer{[this] { tick(); }},
    tick_timer_without_osd_lock{[this] { tick_without_osd_lock(); }}
{}

void OSD::tick()
{}

void OSD::tick_without_osd_lock()
{}

int OSD::init()
{
  int r = tp.submit([this] {
    if (int r = store->mount(); r < 0) {
      std::cerr << __func__ << ": unable to mount object store" << std::endl;
      return r;
    }
    journal_is_rotational = store->is_journal_rotational();
    service.meta_ch = store->open_collection(coll_t::meta());
    return 0;
  }).get();
}

seastar::future<> OSD::ms_dispatch(ConnectionRef conn, MessageRef m)
{
}
