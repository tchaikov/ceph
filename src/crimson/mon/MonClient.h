// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#pragma once

#include <memory>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timer.hh>

#include "auth/AuthClientHandler.h"
#include "auth/AuthMethodList.h"
#include "auth/KeyRing.h"
#include "auth/RotatingKeyRing.h"

#include "common/lock_policy.h"

#include "crimson/net/Connection.h"
#include "crimson/net/Dispatcher.h"

#include "messages/MAuthReply.h"
#include "mon/MonMap.h"

#include "mon/MonSub.h"

template<typename Message> using Ref = boost::intrusive_ptr<Message>;
namespace ceph::net {
  class Messenger;
}

struct MMonMap;
struct MMonSubscribeAck;
struct MMonGetVersionReply;
struct MMonCommandAck;
struct MLogAck;
struct MConfig;

namespace ceph::mon {

class Connection {
public:
  Connection(ceph::net::ConnectionRef conn,
	     KeyRing* keyring);
  seastar::future<> handle_auth_reply(Ref<MAuthReply> m);
  seastar::future<> authenticate(epoch_t epoch,
				 const EntityName& name,
				 const AuthMethodList& auth_methods,
				 uint32_t want_keys);
  seastar::future<> close();
  bool is_my_peer(const entity_addr_t& addr) const;

  seastar::future<> renew_tickets();
  ceph::net::ConnectionRef get_conn();

private:
  seastar::future<> setup_session(epoch_t epoch,
				  const AuthMethodList& auth_methods,
				  const EntityName& name);
  std::unique_ptr<AuthClientHandler> create_auth(Ref<MAuthReply> m,
						 const EntityName& name,
						 uint32_t want_keys);
  seastar::future<bool> do_auth();

private:
  bool closed = false;
  seastar::promise<Ref<MAuthReply>> reply;
  ceph::net::ConnectionRef conn;
  std::unique_ptr<AuthClientHandler> auth;
  using RotatingKeyRingT = RotatingKeyRing<ceph::LockPolicy::SINGLE>;
  RotatingKeyRingT rotating_keyring;
  uint64_t global_id;
};

class Client : public ceph::net::Dispatcher {
  const EntityName entity_name;
  KeyRing keyring;
  AuthMethodList auth_methods;
  const uint32_t want_keys;

  MonMap monmap;
  seastar::promise<MessageRef> reply;
  std::unique_ptr<Connection> active_con;
  std::vector<Connection> pending_conns;
  seastar::timer<seastar::lowres_clock> timer;
  seastar::gate tick_gate;

  ceph::net::Messenger& msgr;

  // commands
  using get_version_t = seastar::future<version_t, version_t>;

  ceph_tid_t last_version_req_id = 0;
  std::map<ceph_tid_t, typename get_version_t::promise_type> version_reqs;

  ceph_tid_t last_mon_command_id = 0;
  using command_result_t =
    seastar::future<std::int32_t, string, ceph::bufferlist>;
  std::map<ceph_tid_t, typename command_result_t::promise_type> mon_commands;

  MonSub sub;

public:
  Client(const EntityName& name,
	 ceph::net::Messenger& messenger);
  seastar::future<> load_keyring();
  seastar::future<> build_initial_map();
  seastar::future<> authenticate(std::chrono::seconds seconds);
  seastar::future<> stop();
  get_version_t get_version(const std::string& map);
  command_result_t run_command(const std::vector<std::string>& cmd,
			       const bufferlist& bl);

private:
  void tick();

  seastar::future<> ms_dispatch(ceph::net::ConnectionRef conn,
				MessageRef m) override;
  seastar::future<> ms_handle_reset(ceph::net::ConnectionRef conn) override;

  seastar::future<> handle_monmap(ceph::net::ConnectionRef conn,
				  Ref<MMonMap> m);
  seastar::future<> handle_auth_reply(ceph::net::ConnectionRef conn,
				      Ref<MAuthReply> m);
  seastar::future<> handle_subscribe_ack(Ref<MMonSubscribeAck> m);
  seastar::future<> handle_get_version_reply(Ref<MMonGetVersionReply> m);
  seastar::future<> handle_mon_command_ack(Ref<MMonCommandAck> m);
  seastar::future<> handle_log_ack(Ref<MLogAck> m);
  seastar::future<> handle_config(Ref<MConfig> m);

private:
  bool is_hunting() const;
  seastar::future<> reopen_session(int rank);
  std::vector<unsigned> get_random_mons(unsigned n) const;
  seastar::future<> _add_conn(unsigned rank, uint64_t global_id);
};

} // namespace ceph::mon
