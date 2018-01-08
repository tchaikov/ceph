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

#include "SocketMessenger.h"
#include "SocketConnection.h"
#include "Dispatcher.h"
#include "msg/Message.h"

using namespace ceph::net;

SocketMessenger::SocketMessenger()
{
}

void SocketMessenger::bind(const entity_addr_t& addr)
{
  if (addr.get_family() != AF_INET) {
    throw std::system_error(EAFNOSUPPORT, std::generic_category());
  }

  my_addr = addr;

  seastar::socket_address address(addr.in4_addr());
  seastar::listen_options lo;
  lo.reuse_address = true;
  listener = seastar::listen(address, lo);
}

seastar::future<> SocketMessenger::dispatch(ConnectionRef conn)
{
  connections.push_back(conn);

  return seastar::repeat([=] {
      return conn->read_header()
        .then([=] (ceph_msg_header header) {
          return conn->read_message();
        }).then([=] (MessageRef msg) {
          return dispatcher->ms_dispatch(conn, std::move(msg));
        }).then([] {
          return seastar::stop_iteration::no;
        });
    }).handle_exception_type([=] (const std::system_error& e) {
      if (e.code() == error::connection_aborted ||
          e.code() == error::connection_reset) {
        dispatcher->ms_handle_reset(conn);
      } else if (e.code() == error::read_eof) {
        dispatcher->ms_handle_remote_reset(conn);
      } else {
        throw e;
      }
    });
}

seastar::future<> SocketMessenger::accept(seastar::connected_socket socket,
                                          seastar::socket_address paddr)
{
  // allocate the connection
  entity_addr_t peer_addr;
  peer_addr.set_sockaddr(&paddr.as_posix_sockaddr());
  ConnectionRef conn = new SocketConnection(this, get_myaddr(),
                                            peer_addr, std::move(socket));
  // initiate the handshake
  return conn->server_handshake()
    .handle_exception([conn] (std::exception_ptr eptr) {
      // close the connection before returning errors
      return seastar::make_exception_future<>(eptr)
        .finally([conn] { return conn->close(); });
    }).then([this, conn] {
      dispatcher->ms_handle_accept(conn);
      // dispatch messages until the connection closes or the dispatch
      // queue shuts down
      return dispatch(std::move(conn));
    });
}

seastar::future<> SocketMessenger::start(Dispatcher *disp)
{
  dispatcher = disp;

  // start listening if bind() was called
  if (listener) {
    seastar::repeat([this] {
        return listener->accept()
          .then([this] (seastar::connected_socket socket,
                        seastar::socket_address paddr) {
            // start processing the connection
            accept(std::move(socket), paddr)
              .handle_exception([] (std::exception_ptr eptr) {});
            // don't wait before accepting another
            return seastar::stop_iteration::no;
          });
      }).handle_exception_type([this] (const std::system_error& e) {
        // stop gracefully on connection_aborted
        if (e.code() != error::connection_aborted) {
          throw e;
        }
      });
  }

  return seastar::now();
}

seastar::future<ceph::net::ConnectionRef> SocketMessenger::connect(const entity_addr_t& addr,
                                                        const entity_addr_t& myaddr)
{
  // TODO: look up existing connection
  return seastar::connect(addr.in4_addr())
    .then([=] (seastar::connected_socket socket) {
      ConnectionRef conn = new SocketConnection(this, addr, myaddr,
                                                std::move(socket));
      // complete the handshake before returning to the caller
      return conn->client_handshake()
        .handle_exception([conn] (std::exception_ptr eptr) {
          // close the connection before returning errors
          return seastar::make_exception_future<>(eptr)
            .finally([conn] { return conn->close(); });
        }).then([=] {
          dispatcher->ms_handle_connect(conn);
          // dispatch replies on this connection
          dispatch(conn)
            .handle_exception([] (std::exception_ptr eptr) {});
          return conn;
        });
    });
}

seastar::future<> SocketMessenger::shutdown()
{
  if (listener) {
    listener->abort_accept();
  }
  return seastar::parallel_for_each(connections.begin(), connections.end(),
    [this] (ConnectionRef conn) {
      return conn->close();
    }).finally([this] { connections.clear(); });
}
