// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#pragma once

class OSDMap;

class OSDState {

  enum class State {
    INITIALIZING,
    PREBOOT,
    BOOTING,
    ACTIVE,
    STOPPING,
    WAITING_FOR_HEALTHY,
  };

  State state = State::INITIALIZING;

public:
  bool is_initializing() const {
    return state == State::INITIALIZING;
  }
  bool is_preboot() const {
    return state == State::PREBOOT;
  }
  bool is_booting() const {
    return state == State::BOOTING;
  }
  bool is_active() const {
    return state == State::ACTIVE;
  }
  bool is_stopping() const {
    return state == State::STOPPING;
  }
  bool is_waiting_for_healthy() const {
    return state == State::WAITING_FOR_HEALTHY;
  }
  void set_preboot() {
    state = State::PREBOOT;
  }
  void set_booting() {
    state = State::BOOTING;
  }
  void set_active() {
    state = State::ACTIVE;
  }
  void set_stopping() {
    state = State::STOPPING;
  }
  const char* print() {
    switch (state) {
    case State::INITIALIZING: return "initializing";
    case State::PREBOOT: return "preboot";
    case State::BOOTING: return "booting";
    case State::ACTIVE: return "active";
    case State::STOPPING: return "stopping";
    case State::WAITING_FOR_HEALTHY: return "waiting_for_healthy";
    default: return "???";
    }
  }
};
