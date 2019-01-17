#include "pg.h"

#include "crimson/os/cyan_collection.h"
#include "crimson/os/cyan_store.h"
#include "crimson/osd/pg_backend.h"
#include "crimson/osd/pg_meta.h"
#include "crimson/osd/osd.h"

PG::PG(pg_pool_t&& pool, std::string&& name, ec_profile_t&& ec_profile)
{
  // TODO
}

seastar::future<> PG::handle_message(Ref<Message> m)
{
  return backend->handle_message(m).then([this, m](bool handled) {
    if (handled) {
      return seastar::now();
    } else {
      switch (m->get_type()) {
      case CEPH_MSG_OSD_BACKOFF:
      case MSG_OSD_PG_SCAN:
      case MSG_OSD_PG_BACKFILL:
      case MSG_OSD_PG_BACKFILL_REMOVE:
      case MSG_OSD_SCRUB_RESERVE:
      case MSG_OSD_REP_SCRUB:
      case MSG_OSD_REP_SCRUBMAP:
      case MSG_OSD_PG_UPDATE_LOG_MISSING:
      case MSG_OSD_PG_UPDATE_LOG_MISSING_REPLY:
      default:
        return seastar::now();
      }
    }
  });
}

