// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include <map>
#include <memory>
#include <random>
#include <vector>

#include "msg/Connection.h"

struct EntityName;
struct MAuthReply;
class CephContext;
class AuthClientHandler;
class AuthMethodList;
class Messenger;
class MonMap;
class RotatingKeyRing;


class MonHunter {
public:
  MonHunter(CephContext *cct,
            ConnectionRef conn);
  ~MonHunter();
  bool handle_auth(MAuthReply *m);
  void start(epoch_t epoch,
             const EntityName& entity_name,
             const AuthMethodList& auth_supported);
  bool is_finished() const;

private:
  bool _negotiate(MAuthReply *m);
  int _authenticate(MAuthReply *m);

private:
  CephContext *cct;
  enum class State {
    NONE,
    NEGOTIATING,
    AUTHENTICATING,
    HAVE_SESSION,
  };
  State state = State::NONE;
  ConnectionRef conn;
  std::unique_ptr<AuthClientHandler> auth;
  RotatingKeyRing *rotating_secrets;
  uint64_t global_id;
};

class MonHunters {
public:
  MonHunters(CephContext *cct, uint64_t global_id);
  void add_hunter(Messenger *msgr, const MonMap& monmap, unsigned rank);
  void add_hunters(Messenger *msgr, const MonMap& monmap, int n);
  void start(epoch_t epoch,
             const EntityName& entity_name,
             const AuthMethodList& auth_supported);
  bool handle_auth(MAuthReply *m);
  double get_reopen_interval_multiplier() const;
  bool is_hunting() const;
  int get_auth_error() const;

private:
  void _start_hunting();
  void _finish_hunting();

private:
  CephContext *cct;
  RotatingKeyRing *rotating_secrets;

  std::mt19937 rng;
  std::map<entity_inst_t, MonHunter> hunters;
  double reopen_interval_multiplier;
  bool had_a_connection;

  ConnectionRef conn;
  uint64_t global_id;
};
