#include "MonHunter.h"

#include "auth/AuthClientHandler.h"
#include "auth/AuthMethodList.h"
#include "auth/RotatingKeyRing.h"
#include "messages/MAuth.h"
#include "messages/MAuthReply.h"
#include "MonMap.h"

MonHunter::MonHunter(CephContext *cct, ConnectionRef conn)
  : cct(cct), conn(conn)
{}

MonHunter::~MonHunter()
{
  conn->mark_down();
}

bool MonHunter::is_finished() const
{
  return state == State::NONE;
}

void MonHunter::start(epoch_t epoch,
                      const EntityName& entity_name,
                      const AuthMethodList& auth_supported)
{
  // restart authentication handshake
  state = State::NEGOTIATING;

  // send an initial keepalive to ensure our timestamp is valid by the
  // time we are in an OPENED state (by sequencing this before
  // authentication).
  conn->send_keepalive();

  auto m = new MAuth;
  m->protocol = 0;
  m->monmap_epoch = epoch;
  __u8 struct_v = 1;
  ::encode(struct_v, m->auth_payload);
  ::encode(auth_supported.get_supported_set(), m->auth_payload);
  ::encode(entity_name, m->auth_payload);
  ::encode(global_id, m->auth_payload);
  conn->send_message(m);
}

bool MonHunter::handle_auth(MAuthReply *m)
{
  if (state == State::NEGOTIATING) {
    if (_negotiate(m)) {
      state = State::AUTHENTICATING;
    }
  }
  if (state == State::AUTHENTICATING) {
    switch (_authenticate(m)) {
    case -EAGAIN:
      break;
    case 0:
      state = State::HAVE_SESSION;
      break;
    default:
      // something worse, give up.
      state = State::NONE;
      break;
    }
  }
  return state == State::HAVE_SESSION;
}

bool MonHunter::_negotiate(MAuthReply *m)
{
  if (auth && (int)m->protocol == auth->get_protocol()) {
    auth->reset();
    return true;
  } else {
    auth.reset(get_auth_client_handler(cct, m->protocol, rotating_secrets));
    if (auth) {
      auth->set_want_keys(want_keys);
      auth->init(entity_name);
      auth->set_global_id(global_id);
      return true;
    } else {
      ldout(cct, 10) << "no handler for protocol " << m->protocol << dendl;
      if (m->result == -ENOTSUP) {
        ldout(cct, 10) << "none of our auth protocols are supported by the server"
                       << dendl;
        authenticate_err = m->result;
      }
      return false;
    }
  }
}

int MonHunter::_authenticate(MAuthReply *m)
{
  assert(auth);

  if (m->global_id && m->global_id != global_id) {
    global_id = m->global_id;
    auth->set_global_id(global_id);
    ldout(cct, 10) << "my global_id is " << m->global_id << dendl;
  }

  int ret = auth->handle_response(m->result, p);

  if (ret == -EAGAIN) {
    auto msg = new MAuth;
    msg->protocol = auth->get_protocol();
    auth->prepare_build_request();
    ret = auth->build_request(msg->auth_payload);
    conn->send_message(msg);
  }
  return ret;
}

MonHunters::MonHunters(CephContext *cct, uint64_t global_id)
  : cct(cct),
    global_id(global_id),
    rng(getpid())
{}

double MonHunters::get_reopen_interval_multiplier() const
{
  return reopen_interval_multiplier;
}

bool MonHunters::is_hunting() const {
  return !hunters.empty();
}

int MonHunters::get_auth_error() const {
  return authenticate_err;
}

void MonHunters::add_hunter(Messenger *msgr, const MonMap& monmap,
                            unsigned rank)
{
  auto peer = monmap.get_addr(rank);
  auto conn = msgr->get_connection(monmap.get_inst(rank));
  hunters.emplace(peer, cct, conn);
  ldout(cct, 10) << "picked mon." << monmap.get_name(rank)
                 << " con " << conn
                 << " addr " << conn->get_peer_addr()
                 << dendl;
}

void MonHunters::add_hunters(Messenger *msgr, const MonMap& monmap)
{
  unsigned n = cct->_conf->mon_client_hunt_parallel;
  if (n <= 0 || n > monmap.size()) {
    n = monmap.size();
  }
  vector<unsigned> ranks(n);
  for (unsigned i = 0; i < n; i++) {
    ranks[i] = i;
  }
  std::shuffle(ranks.begin(), ranks.end(), rng);
  for (int i = 0; i < n; i++) {
    add_hunter(msgr, monmap, ranks[i]);
  }
}

void MonHunters::start(epoch_t epoch,
                       const EntityName& entity_name,
                       const AuthMethodList& auth_supported)
{
  for (auto& hunter : hunters) {
    hunter.start(epoch, entity_name, auth_methods);
  }
  _start_hunting();
}

bool MonHunters::handle_auth(MAuthReply *m)
{
  auto peer = m->get_source_inst();
  if (!hunters.count()) {
    ldout(cct, 10) << "discarding stray monitor message " << *m << dendl;
    return false;
  }
  auto& hunter = hunters[peer];
  if (hunter.handle_auth(m)) {
    conn = hunter.conn;
    global_id = hunter.global_id;
  }
  if (hunter.is_finished()) {
    hunters.erase(peer);
    if (hunters.empty()) {
      _finish_hunting();
    }
  }
  return conn;
}

void MonHunters::_start_hunting()
{
  // adjust timeouts if necessary
  if (!had_a_connection)
    return;
  reopen_interval_multiplier *= cct->_conf->mon_client_hunt_interval_backoff;
  if (reopen_interval_multiplier >
      cct->_conf->mon_client_hunt_interval_max_multiple)
    reopen_interval_multiplier =
      cct->_conf->mon_client_hunt_interval_max_multiple;
  }
}

void MonHunters::_finish_hunting()
{
  had_a_connection = true;
  reopen_interval_multiplier /= 2.0;
  if (reopen_interval_multiplier < 1.0) {
    reopen_interval_multiplier = 1.0;
  }
}
