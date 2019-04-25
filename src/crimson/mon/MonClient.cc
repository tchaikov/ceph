#include "MonClient.h"

#include <random>

#include <seastar/core/future-util.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/util/log.hh>

#include "auth/AuthClientHandler.h"
#include "auth/RotatingKeyRing.h"

#include "common/hostname.h"

#include "crimson/auth/KeyRing.h"
#include "crimson/common/config_proxy.h"
#include "crimson/common/log.h"
#include "crimson/net/Connection.h"
#include "crimson/net/Errors.h"
#include "crimson/net/Messenger.h"

#include "messages/MAuth.h"
#include "messages/MAuthReply.h"
#include "messages/MConfig.h"
#include "messages/MLogAck.h"
#include "messages/MMonCommand.h"
#include "messages/MMonCommandAck.h"
#include "messages/MMonGetMap.h"
#include "messages/MMonGetVersion.h"
#include "messages/MMonGetVersionReply.h"
#include "messages/MMonMap.h"
#include "messages/MMonSubscribe.h"
#include "messages/MMonSubscribeAck.h"

namespace {
  seastar::logger& logger()
  {
    return ceph::get_logger(ceph_subsys_monc);
  }

  template<typename Message, typename... Args>
  Ref<Message> make_message(Args&&... args)
  {
    return {new Message{std::forward<Args>(args)...}, false};
  }
}

namespace ceph::mon {


class Connection {
public:
  Connection(const AuthRegistry& auth_registry,
             ceph::net::ConnectionRef conn,
             KeyRing* keyring);
  seastar::future<> handle_auth_reply(Ref<MAuthReply> m);
  // v1
  seastar::future<> authenticate_v1(epoch_t epoch,
                                    const EntityName& name,
                                    uint32_t want_keys);
  // v2
  seastar::future<> authenticate_v2();
  auth::AuthClient::auth_request_t
  get_auth_request(const EntityName& name,
                   uint32_t want_keys);
  using secret_t = string;
  tuple<CryptoKey, secret_t, bufferlist>
  handle_auth_reply_more(const ceph::buffer::list& bl);
  tuple<CryptoKey, secret_t, int>
  handle_auth_done(uint64_t new_global_id,
                   const ceph::buffer::list& bl);
  int handle_auth_bad_method(uint32_t old_auth_method,
                             int result,
                             const std::vector<uint32_t>& allowed_methods,
                             const std::vector<uint32_t>& allowed_modes);

  // v1 and v2
  seastar::future<> close();
  bool is_my_peer(const entity_addr_t& addr) const;
  AuthAuthorizer* get_authorizer(peer_type_t peer) const;
  KeyStore& get_keys();
  seastar::future<> renew_tickets();
  ceph::net::ConnectionRef get_conn();

private:
  seastar::future<> setup_session(epoch_t epoch,
                                  const EntityName& name);
  std::unique_ptr<AuthClientHandler> create_auth(ceph::auth::method_t,
                                                 uint64_t global_id,
                                                 const EntityName& name,
                                                 uint32_t want_keys);
  seastar::future<bool> do_auth();

private:
  bool closed = false;
  // v1
  seastar::promise<Ref<MAuthReply>> reply;
  // v2
  using clock_t = seastar::lowres_system_clock;
  clock_t::time_point auth_start;
  ceph::auth::method_t auth_method = 0;
  seastar::promise<> auth_done;
  // v1 and v2
  const AuthRegistry& auth_registry;
  ceph::net::ConnectionRef conn;
  std::unique_ptr<AuthClientHandler> auth;
  RotatingKeyRing rotating_keyring;
  uint64_t global_id;
};

Connection::Connection(const AuthRegistry& auth_registry,
                       ceph::net::ConnectionRef conn,
                       KeyRing* keyring)
  : auth_registry{auth_registry},
    conn{conn},
    rotating_keyring{nullptr, CEPH_ENTITY_TYPE_OSD, keyring}
{}

seastar::future<> Connection::handle_auth_reply(Ref<MAuthReply> m)
{
  reply.set_value(m);
  return seastar::now();
}

seastar::future<> Connection::renew_tickets()
{
  if (auth->need_tickets()) {
    return do_auth().then([](bool success) {
      if (!success)  {
        throw std::system_error(make_error_code(
          ceph::net::error::negotiation_failure));
      }
    });
  }
  return seastar::now();
}

AuthAuthorizer* Connection::get_authorizer(peer_type_t peer) const
{
  if (auth) {
    return auth->build_authorizer(peer);
  } else {
    return nullptr;
  }
}

KeyStore& Connection::get_keys() {
  return rotating_keyring;
}

std::unique_ptr<AuthClientHandler>
Connection::create_auth(ceph::auth::method_t protocol,
                        uint64_t global_id,
                        const EntityName& name,
                        uint32_t want_keys)
{
  static CephContext cct;
  std::unique_ptr<AuthClientHandler> auth;
  auth.reset(AuthClientHandler::create(&cct,
                                       protocol,
                                       &rotating_keyring));
  if (!auth) {
    logger().error("no handler for protocol {}", protocol);
    throw std::system_error(make_error_code(
      ceph::net::error::negotiation_failure));
  }
  auth->init(name);
  auth->set_want_keys(want_keys);
  auth->set_global_id(global_id);
  return auth;
}

seastar::future<>
Connection::setup_session(epoch_t epoch,
                          const EntityName& name)
{
  auto m = make_message<MAuth>();
  m->protocol = 0;
  m->monmap_epoch = epoch;
  __u8 struct_v = 1;
  encode(struct_v, m->auth_payload);
  std::vector<ceph::auth::method_t> auth_methods;
  auth_registry.get_supported_methods(conn->get_peer_type(), &auth_methods);
  encode(auth_methods, m->auth_payload);
  encode(name, m->auth_payload);
  encode(global_id, m->auth_payload);
  return conn->send(m);
}

seastar::future<bool> Connection::do_auth()
{
  auto m = make_message<MAuth>();
  m->protocol = auth->get_protocol();
  auth->prepare_build_request();
  if (int ret = auth->build_request(m->auth_payload); ret) {
    logger().error("missing/bad key for '{}'",
                   ceph::common::local_conf()->name);
    throw std::system_error(make_error_code(
      ceph::net::error::negotiation_failure));
  }
  logger().info("sending {}", *m);
  return conn->send(m).then([this] {
    logger().info("waiting");
    return reply.get_future();
  }).then([this] (Ref<MAuthReply> m) {
    logger().info("mon {} => {} returns {}: {}",
                   conn->get_messenger()->get_myaddr(),
                   conn->get_peer_addr(), *m, m->result);
    reply = {};
    auto p = m->result_bl.cbegin();
    auto ret = auth->handle_response(m->result, p,
				     nullptr, nullptr);
    if (ret != 0 && ret != -EAGAIN) {
      throw std::system_error(make_error_code(
        ceph::net::error::negotiation_failure));
    }
    return seastar::make_ready_future<bool>(ret == 0);
  });
}

seastar::future<>
Connection::authenticate_v1(epoch_t epoch,
                            const EntityName& name,
                            uint32_t want_keys)
{
  return conn->keepalive().then([epoch, name, this] {
    return setup_session(epoch, name);
  }).then([this] {
    return reply.get_future();
  }).then([name, want_keys, this](Ref<MAuthReply> m) {
    reply = {};
    if (m->global_id != global_id) {
      // it's a new session
      global_id = m->global_id;
      auth->set_global_id(global_id);
      auth->reset();
    }
    auth = create_auth(m->protocol, m->global_id, name, want_keys);
    global_id = m->global_id;
    switch (auto p = m->result_bl.cbegin();
            auth->handle_response(m->result, p,
				  nullptr, nullptr)) {
    case 0:
      // none
      return seastar::now();
    case -EAGAIN:
      // cephx
      return seastar::repeat([this] {
        return do_auth().then([](bool success) {
          return seastar::make_ready_future<seastar::stop_iteration>(
            success ?
            seastar::stop_iteration::yes:
            seastar::stop_iteration::no);
          });
        });
    default:
      ceph_assert_always(0);
    }
  });
}

seastar::future<> Connection::authenticate_v2()
{
  auth_start = seastar::lowres_system_clock::now();
  return conn->send(make_message<MMonGetMap>()).then([this] {
    return auth_done.get_future();
  });
}

auth::AuthClient::auth_request_t
Connection::get_auth_request(const EntityName& entity_name,
                             uint32_t want_keys)
{
  // choose method
  auth_method = [&] {
    std::vector<ceph::auth::method_t> methods;
    auth_registry.get_supported_methods(conn->get_peer_type(), &methods);
    if (methods.empty()) {
      logger().info("get_auth_request no methods is supported");
      throw ceph::auth::error("no methods is supported");
    }
    return methods.front();
  }();

  std::vector<uint32_t> modes;
  auth_registry.get_supported_modes(conn->get_peer_type(), auth_method,
                                    &modes);
  logger().info("method {} preferred_modes {}", auth_method, modes);
  if (modes.empty()) {
    throw ceph::auth::error("no modes is supported");
  }
  auth = create_auth(auth_method, global_id, entity_name, want_keys);

  using ceph::encode;
  bufferlist bl;
  // initial request includes some boilerplate...
  encode((char)AUTH_MODE_MON, bl);
  encode(entity_name, bl);
  encode(global_id, bl);
  // and (maybe) some method-specific initial payload
  auth->build_initial_request(&bl);
  return {auth_method, modes, bl};
}

tuple<CryptoKey, Connection::secret_t, bufferlist>
Connection::handle_auth_reply_more(const ceph::buffer::list& payload)
{
  CryptoKey session_key;
  secret_t connection_secret;
  bufferlist reply;
  auto p = payload.cbegin();
  int r = auth->handle_response(0, p, &session_key,	&connection_secret);
  if (r == -EAGAIN) {
    auth->prepare_build_request();
    auth->build_request(reply);
    logger().info(" responding with {} bytes", reply.length());
    return {session_key, connection_secret, reply};
  } else if (r < 0) {
    logger().error(" handle_response returned {}",  r);
    throw ceph::auth::error("unable to build auth");
  } else {
    logger().info("authenticated!");
    std::terminate();
  }
}

tuple<CryptoKey, Connection::secret_t, int>
Connection::handle_auth_done(uint64_t new_global_id,
                             const ceph::buffer::list& payload)
{
  global_id = new_global_id;
  auth->set_global_id(global_id);
  auto p = payload.begin();
  CryptoKey session_key;
  secret_t connection_secret;
  int r = auth->handle_response(0, p, &session_key, &connection_secret);
  conn->set_last_keepalive_ack(auth_start);
  auth_done.set_value();
  return {session_key, connection_secret, r};
}

int Connection::handle_auth_bad_method(uint32_t old_auth_method,
                                       int result,
                                       const std::vector<uint32_t>& allowed_methods,
                                       const std::vector<uint32_t>& allowed_modes)
{
  logger().info("old_auth_method {} result {} allowed_methods {}",
                old_auth_method, cpp_strerror(result), allowed_methods);
  std::vector<uint32_t> auth_supported;
  auth_registry.get_supported_methods(conn->get_peer_type(), &auth_supported);
  auto p = std::find(auth_supported.begin(), auth_supported.end(),
                     old_auth_method);
  assert(p != auth_supported.end());
  p = std::find_first_of(std::next(p), auth_supported.end(),
                         allowed_methods.begin(), allowed_methods.end());
  if (p == auth_supported.end()) {
    logger().error("server allowed_methods {} but i only support {}",
                   allowed_methods, auth_supported);
    auth_done.set_exception(std::system_error(make_error_code(
      ceph::net::error::negotiation_failure)));
    return -EACCES;
  }
  auth_method = *p;
  logger().info("will try {} next", auth_method);
  return 0;
}

seastar::future<> Connection::close()
{
  if (conn && !std::exchange(closed, true)) {
    return conn->close();
  } else {
    return seastar::now();
  }
}

bool Connection::is_my_peer(const entity_addr_t& addr) const
{
  return conn->get_peer_addr() == addr;
}

ceph::net::ConnectionRef Connection::get_conn() {
  return conn;
}

Client::Client(ceph::net::Messenger& messenger,
               ceph::common::AuthHandler& auth_handler)
  // currently, crimson is OSD-only
  : want_keys{CEPH_ENTITY_TYPE_MON |
              CEPH_ENTITY_TYPE_OSD |
              CEPH_ENTITY_TYPE_MGR},
    timer{[this] { tick(); }},
    msgr{messenger},
    auth_registry{&cct},
    auth_handler{auth_handler}
{}

Client::Client(Client&&) = default;
Client::~Client() = default;

seastar::future<> Client::start() {
  auth_registry.refresh_config();
  return load_keyring().then([this] {
    return monmap.build_initial(ceph::common::local_conf(), false);
  }).then([this] {
    return authenticate();
  });
}

seastar::future<> Client::load_keyring()
{
  if (!auth_registry.is_supported_method(msgr.get_mytype(), CEPH_AUTH_CEPHX)) {
    return seastar::now();
  } else {
    return ceph::auth::load_from_keyring(&keyring).then([](KeyRing* keyring) {
      return ceph::auth::load_from_keyfile(keyring);
    }).then([](KeyRing* keyring) {
      return ceph::auth::load_from_key(keyring);
    }).then([](KeyRing*) {
      return seastar::now();
    });
  }
}

void Client::tick()
{
  seastar::with_gate(tick_gate, [this] {
    return active_con->renew_tickets();
  });
}

bool Client::is_hunting() const {
  return !active_con;
}

seastar::future<>
Client::ms_dispatch(ceph::net::Connection* conn, MessageRef m)
{
  // we only care about these message types
  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
    return handle_monmap(conn, boost::static_pointer_cast<MMonMap>(m));
  case CEPH_MSG_AUTH_REPLY:
    return handle_auth_reply(
      conn, boost::static_pointer_cast<MAuthReply>(m));
  case CEPH_MSG_MON_SUBSCRIBE_ACK:
    return handle_subscribe_ack(
      boost::static_pointer_cast<MMonSubscribeAck>(m));
  case CEPH_MSG_MON_GET_VERSION_REPLY:
    return handle_get_version_reply(
      boost::static_pointer_cast<MMonGetVersionReply>(m));
  case MSG_MON_COMMAND_ACK:
    return handle_mon_command_ack(
      boost::static_pointer_cast<MMonCommandAck>(m));
  case MSG_LOGACK:
    return handle_log_ack(
      boost::static_pointer_cast<MLogAck>(m));
  case MSG_CONFIG:
    return handle_config(
      boost::static_pointer_cast<MConfig>(m));
  default:
    return seastar::now();
  }
}

seastar::future<> Client::ms_handle_reset(ceph::net::ConnectionRef conn)
{
  auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                            [peer_addr = conn->get_peer_addr()](auto& mc) {
                              return mc.is_my_peer(peer_addr);
                            });
  if (found != pending_conns.end()) {
    logger().warn("pending conn reset by {}", conn->get_peer_addr());
    return found->close();
  } else if (active_con && active_con->is_my_peer(conn->get_peer_addr())) {
    logger().warn("active conn reset {}", conn->get_peer_addr());
    active_con.reset();
    return reopen_session(-1);
  } else {
    logger().error("unknown reset from {}", conn->get_peer_addr());
    return seastar::now();
  }
}

std::pair<std::vector<uint32_t>, std::vector<uint32_t>>
Client::get_supported_auth_methods(int peer_type)
{
    std::vector<uint32_t> methods;
    std::vector<uint32_t> modes;
    auth_registry.get_supported_methods(peer_type, &methods, &modes);
    return {methods, modes};
}

uint32_t Client::pick_con_mode(int peer_type,
                               uint32_t auth_method,
                               const std::vector<uint32_t>& preferred_modes)
{
  return auth_registry.pick_mode(peer_type, auth_method, preferred_modes);
}

AuthAuthorizeHandler* Client::get_auth_authorize_handler(int peer_type,
                                                         int auth_method)
{
  return auth_registry.get_handler(peer_type, auth_method);
}


int Client::handle_auth_request(ceph::net::ConnectionRef con,
                                AuthConnectionMetaRef auth_meta,
                                bool more,
                                uint32_t auth_method,
                                const ceph::bufferlist& payload,
                                ceph::bufferlist *reply)
{
  // for some channels prior to nautilus (osd heartbeat), we tolerate the lack of
  // an authorizer.
  if (payload.length() == 0 && !auth_handler.require_authorizer()) {
    auth_handler.handle_authentication(name, global_id, caps_info);
    return 1;
  }
  auth_meta->auth_mode = payload[0];
  if (auth_meta->auth_mode < AUTH_MODE_AUTHORIZER ||
      auth_meta->auth_mode > AUTH_MODE_AUTHORIZER_MAX) {
    return -EACCES;
  }
  AuthAuthorizeHandler* ah = get_auth_authorize_handler(con->get_peer_type(),
                                                        auth_method);
  if (!ah) {
    logger().error("no AuthAuthorizeHandler found for auth method: {}",
                   auth_method);
    return -EOPNOTSUPP;
  }
  ceph_assert(active_con);
  bool was_challenge = (bool)auth_meta->authorizer_challenge;
  EntityName name;
  uint64_t global_id;
  AuthCapsInfo caps_info;
  bool is_valid = ah->verify_authorizer(
    nullptr,
    &active_con->get_keys(),
    payload,
    auth_meta->get_connection_secret_length(),
    reply,
    &name,
    &global_id,
    &caps_info,
    &auth_meta->session_key,
    &auth_meta->connection_secret,
    &auth_meta->authorizer_challenge);
  if (is_valid) {
    auth_handler.handle_authentication(name, global_id, caps_info);
    return 1;
  }
  if (!more && !was_challenge && auth_meta->authorizer_challenge) {
    logger().info("added challenge on {}", con);
    return 0;
  } else {
    logger().info("bad authorizer on {}", con);
    return -EACCES;
  }
}

auth::AuthClient::auth_request_t
Client::get_auth_request(ceph::net::ConnectionRef con,
                         AuthConnectionMetaRef auth_meta)
{
  logger().info("get_auth_request(con={}, auth_method={})",
                con, auth_meta->auth_method);
  // connection to mon?
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                              [peer_addr = con->get_peer_addr()](auto& mc) {
                                return mc.is_my_peer(peer_addr);
                              });
    if (found == pending_conns.end()) {
      throw ceph::auth::error{"unknown connection"};
    }
    return found->get_auth_request(entity_name, want_keys);
  } else {
    // generate authorizer
    if (!active_con) {
      logger().error(" but no auth handler is set up");
      throw ceph::auth::error("no auth available");
    }
    auto authorizer = active_con->get_authorizer(con->get_peer_type());
    if (!authorizer) {
      logger().error("failed to build_authorizer for type {}",
                     ceph_entity_type_name(con->get_peer_type()));
      throw ceph::auth::error("unable to build auth");
    }
    auth_meta->authorizer.reset(authorizer);
    auth_meta->auth_method = authorizer->protocol;
    vector<uint32_t> modes;
    auth_registry.get_supported_modes(con->get_peer_type(),
                                      auth_meta->auth_method,
                                      &modes);
    return {authorizer->protocol, modes, authorizer->bl};
  }
}

 ceph::bufferlist Client::handle_auth_reply_more(ceph::net::ConnectionRef conn,
                                                AuthConnectionMetaRef auth_meta,
                                                const bufferlist& bl)
{
  if (conn->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                              [peer_addr = conn->get_peer_addr()](auto& mc) {
                                return mc.is_my_peer(peer_addr);
                              });
    if (found == pending_conns.end()) {
      throw ceph::auth::error{"unknown connection"};
    }
    bufferlist reply;
    tie(auth_meta->session_key, auth_meta->connection_secret, reply) =
      found->handle_auth_reply_more(bl);
    return reply;
  } else {
    // authorizer challenges
    if (!active_con || !auth_meta->authorizer) {
      logger().error("no authorizer?");
      throw ceph::auth::error("no auth available");
    }
    auth_meta->authorizer->add_challenge(&cct, bl);
    return auth_meta->authorizer->bl;
  }
}

int Client::handle_auth_done(ceph::net::ConnectionRef conn,
                             AuthConnectionMetaRef auth_meta,
                             uint64_t global_id,
                             uint32_t con_mode,
                             const bufferlist& bl)
{
  if (conn->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                              [peer_addr = conn->get_peer_addr()](auto& mc) {
                                return mc.is_my_peer(peer_addr);
                              });
    if (found == pending_conns.end()) {
      return -ENOENT;
    }
    int r = 0;
    tie(auth_meta->session_key, auth_meta->connection_secret, r) =
      found->handle_auth_done(global_id, bl);
    return r;
  } else {
    // verify authorizer reply
    auto p = bl.begin();
    if (!auth_meta->authorizer->verify_reply(p, &auth_meta->connection_secret)) {
      logger().error("failed verifying authorizer reply");
      return -EACCES;
    }
    auth_meta->session_key = auth_meta->authorizer->session_key;
    return 0;
  }
}

 // Handle server's indication that the previous auth attempt failed
int Client::handle_auth_bad_method(ceph::net::ConnectionRef conn,
                                   AuthConnectionMetaRef auth_meta,
                                   uint32_t old_auth_method,
                                   int result,
                                   const std::vector<uint32_t>& allowed_methods,
                                   const std::vector<uint32_t>& allowed_modes)
{
  if (conn->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                              [peer_addr = conn->get_peer_addr()](auto& mc) {
                                return mc.is_my_peer(peer_addr);
                              });
    if (found != pending_conns.end()) {
      return found->handle_auth_bad_method(old_auth_method, result,
                                           allowed_methods, allowed_modes);
    } else {
      return -ENOENT;
    }
  } else {
    // huh...
    logger().info("hmm, they didn't like {} result {}",
                  old_auth_method, cpp_strerror(result));
    return -EACCES;
  }
}

seastar::future<> Client::handle_monmap(ceph::net::Connection* conn,
                                        Ref<MMonMap> m)
{
  monmap.decode(m->monmapbl);
  const auto peer_addr = conn->get_peer_addr();
  auto cur_mon = monmap.get_name(peer_addr);
  logger().info("got monmap {}, mon.{}, is now rank {}",
                 monmap.epoch, cur_mon, monmap.get_rank(cur_mon));
  sub.got("monmap", monmap.get_epoch());

  if (monmap.get_addr_name(peer_addr, cur_mon)) {
    return seastar::now();
  } else {
    logger().warn("mon.{} went away", cur_mon);
    return reopen_session(-1);
  }
}

seastar::future<> Client::handle_auth_reply(ceph::net::Connection* conn,
                                               Ref<MAuthReply> m)
{
  logger().info("mon {} => {} returns {}: {}",
                conn->get_messenger()->get_myaddr(),
                conn->get_peer_addr(), *m, m->result);
  auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                            [peer_addr = conn->get_peer_addr()](auto& mc) {
                              return mc.is_my_peer(peer_addr);
                            });
  if (found != pending_conns.end()) {
    return found->handle_auth_reply(m);
  } else if (active_con) {
    return active_con->handle_auth_reply(m);
  } else {
    logger().error("unknown auth reply from {}", conn->get_peer_addr());
    return seastar::now();
  }
}

seastar::future<> Client::handle_subscribe_ack(Ref<MMonSubscribeAck> m)
{
  sub.acked(m->interval);
  return seastar::now();
}

Client::get_version_t Client::get_version(const std::string& map)
{
  auto m = make_message<MMonGetVersion>();
  auto tid = ++last_version_req_id;
  m->handle = tid;
  m->what = map;
  auto& req = version_reqs[tid];
  return active_con->get_conn()->send(m).then([&req] {
    return req.get_future();
  });
}

seastar::future<>
Client::handle_get_version_reply(Ref<MMonGetVersionReply> m)
{
  if (auto found = version_reqs.find(m->handle);
      found != version_reqs.end()) {
    auto& result = found->second;
    logger().trace("{}: {} returns {}",
                 __func__, m->handle, m->version);
    result.set_value(m->version, m->oldest_version);
    version_reqs.erase(found);
  } else {
    logger().warn("{}: version request with handle {} not found",
                __func__, m->handle);
  }
  return seastar::now();
}

seastar::future<> Client::handle_mon_command_ack(Ref<MMonCommandAck> m)
{
  const auto tid = m->get_tid();
  if (auto found = mon_commands.find(tid);
      found != mon_commands.end()) {
    auto& result = found->second;
    logger().trace("{} {}", __func__, tid);
    result.set_value(m->r, m->rs, std::move(m->get_data()));
    mon_commands.erase(found);
  } else {
    logger().warn("{} {} not found", __func__, tid);
  }
  return seastar::now();
}

seastar::future<> Client::handle_log_ack(Ref<MLogAck> m)
{
  // XXX
  return seastar::now();
}

seastar::future<> Client::handle_config(Ref<MConfig> m)
{
  return ceph::common::local_conf().set_mon_vals(m->config);
}

std::vector<unsigned> Client::get_random_mons(unsigned n) const
{
  uint16_t min_priority = std::numeric_limits<uint16_t>::max();
  for (const auto& m : monmap.mon_info) {
    if (m.second.priority < min_priority) {
      min_priority = m.second.priority;
    }
  }
  vector<unsigned> ranks;
  for (auto [name, info] : monmap.mon_info) {
    // TODO: #msgr-v2
    if (info.public_addrs.legacy_addr().is_blank_ip()) {
      continue;
    }
    if (info.priority == min_priority) {
      ranks.push_back(monmap.get_rank(name));
    }
  }
  std::random_device rd;
  std::default_random_engine rng{rd()};
  std::shuffle(ranks.begin(), ranks.end(), rng);
  if (n == 0 || n > ranks.size()) {
    return ranks;
  } else {
    return {ranks.begin(), ranks.begin() + n};
  }
}

seastar::future<> Client::authenticate()
{
  return reopen_session(-1);
}

seastar::future<> Client::stop()
{
  return tick_gate.close().then([this] {
    if (active_con) {
      return active_con->close();
    } else {
      return seastar::now();
    }
  });
}

seastar::future<> Client::reopen_session(int rank)
{
  vector<unsigned> mons;
  if (rank >= 0) {
    mons.push_back(rank);
  } else {
    const auto parallel =
      ceph::common::local_conf().get_val<uint64_t>("mon_client_hunt_parallel");
    mons = get_random_mons(parallel);
  }
  pending_conns.reserve(mons.size());
  return seastar::parallel_for_each(mons, [this](auto rank) {
#warning fixme
    auto peer = monmap.get_addrs(rank).front();
    logger().info("connecting to mon.{}", rank);
    return msgr.connect(peer, CEPH_ENTITY_TYPE_MON).then([this] (auto xconn) {
      // sharded-messenger compatible mode assumes all connections running
      // in one shard.
      ceph_assert((*xconn)->shard_id() == seastar::engine().cpu_id());
      ceph::net::ConnectionRef conn = xconn->release();
      auto& mc = pending_conns.emplace_back(auth_registry, conn, &keyring);
      if (conn->get_peer_addr().is_msgr2()) {
        return mc.authenticate_v2();
      } else {
        return mc.authenticate_v1(monmap.get_epoch(), entity_name, want_keys)
          .handle_exception([conn](auto ep) {
            return conn->close().then([ep = std::move(ep)] {
              std::rethrow_exception(ep);
            });
          });
      }
    }).then([peer, this] {
      if (!is_hunting()) {
        return seastar::now();
      }
      auto found = std::find_if(pending_conns.begin(), pending_conns.end(),
                                [peer](auto& mc) {
                                  return mc.is_my_peer(peer);
                                });
      ceph_assert_always(found != pending_conns.end());
      active_con.reset(new Connection{std::move(*found)});
      return seastar::parallel_for_each(pending_conns, [] (auto& conn) {
        return conn.close();
      });
    });
  }).then([this] {
    pending_conns.clear();
  });
}

Client::command_result_t
Client::run_command(const std::vector<std::string>& cmd,
                    const bufferlist& bl)
{
  auto m = make_message<MMonCommand>(monmap.fsid);
  auto tid = ++last_mon_command_id;
  m->set_tid(tid);
  m->cmd = cmd;
  m->set_data(bl);
  auto& req = mon_commands[tid];
  return active_con->get_conn()->send(m).then([&req] {
    return req.get_future();
  });
}

seastar::future<> Client::send_message(MessageRef m)
{
  return active_con->get_conn()->send(m);
}

bool Client::sub_want(const std::string& what, version_t start, unsigned flags)
{
  return sub.want(what, start, flags);
}

void Client::sub_got(const std::string& what, version_t have)
{
  sub.got(what, have);
}

void Client::sub_unwant(const std::string& what)
{
  sub.unwant(what);
}

bool Client::sub_want_increment(const std::string& what,
                                version_t start,
                                unsigned flags)
{
  return sub.inc_want(what, start, flags);
}

seastar::future<> Client::renew_subs()
{
  if (!sub.have_new()) {
    logger().warn("{} - empty", __func__);
    return seastar::now();
  }
  logger().trace("{}", __func__);

  auto m = make_message<MMonSubscribe>();
  m->what = sub.get_subs();
  m->hostname = ceph_get_short_hostname();
  return active_con->get_conn()->send(m).then([this] {
    sub.renewed();
  });
}

} // namespace ceph::mon
