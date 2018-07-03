// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 

#pragma once

#include <core/timer.hh>

class MonConnection : public ceph::net::Dispatcher {
public:
  MonConnection(ceph::net::ConnectionRef conn);
  seastar::future<> handle_auth_reply(MAuthReply::Ref m);
  seastar::future<> authenticate();
  seastar::future<> close();
  bool is_my_peer(const entity_addr_t& addr) const {
    return conn->get_peer_addr() == addr;
  }
  
private:
  template<typename ReplyMessage>
  seastar::future<ReplyMessage::Ref> wait_reply() {
    return reply.get_future().then([](MessageRef msg) {
      auto m = boost::static_pointer_cast<ReplyMessage::Ref>(msg);
      return seastar::make_ready_future<ReplyMessage::Ref>(m);
    });
  }
  bool do_auth(MAuthReply::Ref m);
  seastar::future<> setup_session();
  seastar::future<> send_auth_request();
private:
  bool closed = false;
  seastar::promise<MessageRef> reply;
  ceph::net::ConnectionRef conn;
  std::unique_ptr<AuthClientHandler> auth;
  uint64_t global_id;
};

class MonClient : public ceph::net::Dispatcher {
  seastar::promise<MessageRef> reply;
  std::unique_ptr<MonConnection> active_con;
  std::vector<MonConnection> pending_conns;
  seastar::timer<seastar::lowres_clock> timer;

public:
  seastar::future<> authenticate(double timeout);
  bool is_hunting();
  MonConnection& find_pending_conn(con)
private:
  seastar::future<> ms_dispatch(ConnectionRef conn, MessageRef m) override;
  seastar::future<> ms_handle_reset(ConnectionRef conn) override;

  seastar::future<> handle_monmap(MMonMap::Ref m);
  seastar::future<> handle_auth_reply(MAuthReply::Ref m);
  seastar::future<> handle_subscribe_ack(MMonSubscribeAck::Ref m);
  seastar::future<> handle_get_version_reply(MMonGetVersionReply::Ref m);
  seastar::future<> handle_mon_command_ack(MMonCommandAck::Ref m);
  seastar::future<> handle_log_ack(MLogAck::Ref m);
  seastar::future<> handle_config(MConfig::Ref m);

private:
  seastar::future<> reopen_session(int rank);
  std::vector<unsigned> get_random_mons(unsigned n) const;
  seastar::future<> _add_conn(unsigned rank, uint64_t global_id);
};
