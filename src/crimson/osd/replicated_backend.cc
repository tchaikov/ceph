#include "replicated_backend.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_osd);
  }
}

seastar::future<bool> ReplicatedBackend::handle_message(Ref<Message> m)
{
  return PGBackend::handle_message(m).then([this](bool handled) {
    if (handled) {
      return seastar::make_ready_future<bool>(true);
    } else {
      switch (m->get_type()) {
      case MSG_OSD_PUSH_PUSH:
      case MSG_OSD_PG_PULL:
      case MSG_OSD_PG_PUSH_REPLY:
      case MSG_OSD_REPOP:
      case MSG_OSD_REPOPREPLY:
      return seastar::make_ready_future<bool>(true);
    default:
      return seastar::make_ready_future<bool>(false);
    }
  });
}
