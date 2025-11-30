# RBD S3 Background Backfill Daemon

set(rbd_backfill_srcs
  BackfillDaemon.cc
  BackfillThrottler.cc
  ImageBackfiller.cc
  ObjectBackfillRequest.cc)

add_library(rbd_backfill_internal STATIC
  ${rbd_backfill_srcs})

add_executable(rbd-backfill
  main.cc)

target_link_libraries(rbd-backfill
  rbd_backfill_internal
  -Wl,--start-group
  rbd_api
  rbd_internal
  rbd_types
  -Wl,--end-group
  journal
  librados
  osdc
  cls_rbd_client
  cls_lock_client
  cls_journal_client
  global
  ceph-common
  CURL::libcurl
  ${ALLOC_LIBS})

install(TARGETS rbd-backfill DESTINATION bin)
