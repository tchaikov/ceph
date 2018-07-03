#include "MonClient.h"

MonConnection::MonConnection(ceph::net::ConnectionRef conn)
  : conn(conn)
{}

seastar::future<> handle_auth_reply(MAuthReply::Ref m)
{
  reply.set_value(m);
}

seastar::future<> MonConnection::send_request()
{
  if (auth) {
    return send_auth_request();
  } else {
    return setup_session();
  }
}

bool MonConnection::do_auth(MAuthReply::Ref m)
{
  if (!auth) {
    auth.reset(get_auth_client_handler(cct, m->protocol, keyring));
    if (!auth) {
      throw std::system_error(make_error_code(error::negotiation_failure));
    }
    auth->set_want_keys(want_keys);
    auth->init(entity_name);
    auth->set_global_id(global_id);
  }
  if (m->global_id != global_id) {
    // it's a new session
    global_id = m->global_id;
    auth->set_global_id(global_id);
    auth->reset();
  }
  auto p = m->result_bl.cbegin();
  int ret = auth->handle_response(m->result, p);
  if (ret == 0) {
    return true;
  } else if (ret == -EAGAIN) {
    return false;
  } else {
    throw std::system_error(make_error_code(error::negotiation_failure));
  }
}

seastar::future<> MonConnection::setup_session()
{
  auto m = new MAuth;
  m->protocol = 0;
  m->monmap_epoch = epoch;
  __u8 struct_v = 1;
  encode(struct_v, m->auth_payload);
  encode(auth_supported.get_supported_set(), m->auth_payload);
  encode(entity_name, m->auth_payload);
  encode(global_id, m->auth_payload);
  return con->send_message(m);
}

seastar::future<> MonConnection::send_auth_request()
{
  auto ma = new MAuth;
  ma->protocol = auth->get_protocol();
  auth->prepare_build_request();
  auth->build_request(ma->auth_payload);
  return con->send_message(ma);
}

seastar::future<> MonConnection::authenticate(SocketConnection conn)
{
  con->send_keepalive()
    .then([this] {
      return seastar::repeat([m] {
        return send_request().then([] {
          return wait_reply<MAuthReply>();
        }).then([](MAuthReply::Ref m) {
          if (do_auth(m)) {
            return seastar::make_ready_future<seastar::stop_iteration>(
              seastar::stop_iteration::yes);
          } else {
            return seastar::make_ready_future<seastar::stop_iteration>(
              seastar::stop_iteration::no);
          }
        });
      });
    });
}

seastar::future<> MonConnection::close()
{
  if (!closed) {
    conn->close();
    closed = true;
  }
}

MonClient::MonClient()
  : timer{[this] { tick(); }}
{}

MonClient::tick()
{
  if (is_hunting()) {
    
  } else {
    
  }
}

bool MonClient::is_hunting() const {
  return !pending_conns.empty();  
}

seastar::future<> MonClient::ms_dispatch(ConnectionRef conn, MessageRef m)
{
  if (my_addr == entity_addr_t{}) {
    my_addr = conn.get_my_addr();
  }
  // we only care about these message types
  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
    return handle_monmap(conn, boost::static_pointer_cast<MMonMap::Ref>(m));
  case CEPH_MSG_AUTH_REPLY:
    return handle_auth_reply(
       conn, boost::static_pointer_cast<MAuthReply::Ref>(m));
  case CEPH_MSG_MON_SUBSCRIBE_ACK:
    return handle_subscribe_ack(
      conn, boost::static_pointer_cast<MMonSubscribeAck::Ref>(m));
  case CEPH_MSG_MON_GET_VERSION_REPLY:
    return handle_get_version_reply(
      conn, boost::static_pointer_cast<MMonGetVersionReply::Ref>(m));
  case MSG_MON_COMMAND_ACK:
    return handle_mon_command_ack(
      conn, boost::static_pointer_cast<MMonCommandAck::Ref>(m));
  case MSG_LOGACK:
    return handle_log_ack(
      conn, boost::static_pointer_cast<MLogAck::Ref>(m));
  case MSG_CONFIG:
    return handle_config(
      conn, boost::static_pointer_cast<MConfig::Ref>(m));
  default:
    return false;
  }
}

seastar::future<> MonClient::ms_handle_reset(ConnectionRef conn)
{
  if (active_con && active_con->is_my_peer(con.get_peer_addr())) {
    active_con.reset();
    return authenticate();
  }
}

seastar::future<> MonClient::handle_auth_reply(ConnectionRef conn,
                                               MAuthReply::Ref m)
{
  if (is_hunting()) {
    auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
      [](auto& mc) { return mc.is_my_peer(conn->get_peer_addr()); });
    if (found == pending_conns.end()) {
      return seastar::make_ready_future<>();
    } else {
      return found->handle_auth_reply(m);
    }
  } else {
    return active_con->handle_auth_reply(m);
  }  
}

std::vector<unsigned> MonClient::get_random_mons(unsigned n) const
{
  uint16_t min_priority = std::numeric_limits<uint16_t>::max();
  for (const auto& m : monmap.mon_info) {
    if (m.second.priority < min_priority) {
      min_priority = m.second.priority;
    }
  }
  vector<unsigned> ranks;
  for (const auto& m : monmap.mon_info) {
    if (m.second.priority == min_priority) {
      ranks.push_back(monmap.get_rank(m.first));
    }
  }
  std::random_device rd;
  std::mt19937 rng(rd());
  std::shuffle(ranks.begin(), ranks.end(), rng);
  if (n == 0 || n > ranks.size()) {
    n = ranks.size();
  }
  return ranks;
}

seastar::future<>
MonClient::authenticate(std::chrono::seconds seconds)
{
  return seastar::with_timeout(
    lowres_clock::now() + seconds, [this] {
      return reopen_session(-1);
    });      
}
          
seastar::future<> MonClient::reopen_session(int rank)
{
  vector<unsigned> mons;
  if (rank >= 0) {
    mons.push_back(rank);
  } else {
    mons = get_random_mons(3); 
  }
  return seastar::parallel_for_each(mons, [this](auto rank) {
    auto peer = monmap.get_addr(rank);
    return msgr.connect(monmap.get_inst(rank)).then([peer](auto conn) {
      auto& mc = pending_conns.emplace_back(conn);
      return mc.authenticate(conn);
    }).then([] {
      return seastar::parallel_for_each(
       pending_conns, [peer, this] (auto& pending_conn) {
         if (pending_conn.is_my_peer(peer)) {
           active_con.reset(new MonConnection{std::move(monc)});
           return seastar::now();
         } else {
           return pending_conn.close();
         }
       });
    }).handle_exception([](auto ep) {
      return conn.close().then([ep = std::move(ep)] {
        std::rethrow_exception(ep);
      });
    });
  });
}
