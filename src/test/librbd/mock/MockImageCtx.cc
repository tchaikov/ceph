// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/librbd/mock/MockImageCtx.h"
#include "test/librbd/mock/MockSafeTimer.h"

static MockSafeTimer *s_timer;
static ceph::mutex *s_timer_lock;

namespace librbd {

MockImageCtx* MockImageCtx::s_instance = nullptr;

void MockImageCtx::set_timer_instance(MockSafeTimer *timer,
                                      ceph::mutex *timer_lock) {
  s_timer = timer;
  s_timer_lock = timer_lock;
}

void MockImageCtx::get_timer_instance(CephContext *cct, MockSafeTimer **timer,
                                      ceph::mutex **timer_lock) {
  *timer = s_timer;
  *timer_lock = s_timer_lock;
}

} // namespace librbd
