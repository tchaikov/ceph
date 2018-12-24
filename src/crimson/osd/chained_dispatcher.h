// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*- 
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/net/Dispatcher.h"

// in existing Messenger, dispatchers are put into a chain as described by
// chain-of-responsibility pattern. we could do the same to stop processing
// the message once any of the dispatchers claims this message, and prevent
// other dispatchers from reading it. but this change is more involved as
// it requires changing the ms_ methods to return a bool. so as an intermediate 
// solution, we are using an observer dispatcher to notify all the interested
// or unintersted parties.
class ChainedDispatchers : public ceph::net::Dispatcher {
  std::deque<Dispatcher*> dispatchers;
public:
  void push_front(Dispatcher* dispatcher) {
    dispatchers.push_front(dispatcher);
  }
  void push_back(Dispatcher* dispatcher) {
    dispatchers.push_back(dispatcher);
  }
  seastar::future<> ms_dispatch(ceph::net::ConnectionRef conn, MessageRef m) override {
    return seastar::do_for_each(dispatchers, [conn, m](Dispatcher* dispatcher) {
      return dispatcher->ms_dispatch(conn, m);
    });
  }
  seastar::future<> ms_handle_accept(ceph::net::ConnectionRef conn) override {
    return seastar::do_for_each(dispatchers, [conn](Dispatcher* dispatcher) {
      return dispatcher->ms_handle_accept(conn);
    });
  }
  seastar::future<> ms_handle_connect(ceph::net::ConnectionRef conn) override {
    return seastar::do_for_each(dispatchers, [conn](Dispatcher* dispatcher) {
      return dispatcher->ms_handle_connect(conn);
    });
  }
  seastar::future<> ms_handle_reset(ceph::net::ConnectionRef conn) override {
    return seastar::do_for_each(dispatchers, [conn](Dispatcher* dispatcher) {
      return dispatcher->ms_handle_reset(conn);
    });
  }
  seastar::future<> ms_handle_remote_reset(ceph::net::ConnectionRef conn) override {
    return seastar::do_for_each(dispatchers, [conn](Dispatcher* dispatcher) {
      return dispatcher->ms_handle_remote_reset(conn);
    });
  }
  seastar::future<std::unique_ptr<AuthAuthorizer>>
  ms_get_authorizer(peer_type_t peer_type, bool force_new) override {
    // since dispatcher returns a nullptr if it does not have the authorizer,
    // let's use the chain-of-responsibility pattern here.
    struct Params {
      peer_type_t peer_type;
      bool force_new;
      std::deque<Dispatcher*>::iterator first, last;
    } params = {peer_type, force_new,
                dispatchers.begin(), dispatchers.end()};
    return seastar::do_with(Params{params}, [this] (Params& params) {
      using result_t = std::unique_ptr<AuthAuthorizer>;
      return seastar::repeat_until_value([&] () {
        auto& first = params.first;
        if (first == params.last) {
          // just give up
          return seastar::make_ready_future<std::optional<result_t>>(result_t{});
        } else {
          return (*first)->ms_get_authorizer(params.peer_type,
                                             params.force_new)
            .then([&] (auto&& auth)-> std::optional<result_t> {
            if (auth) {
              // hooray!
              return std::move(auth);
            } else {
              // try next one
              ++first;
              return {};
            }
          });
        }
      });
    });
  }	 
};
