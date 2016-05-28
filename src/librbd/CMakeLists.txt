add_library(rbd_types STATIC
  journal/Types.cc
  mirroring_watcher/Types.cc
  WatchNotifyTypes.cc)

set(librbd_internal_srcs
  AioCompletion.cc
  AioImageRequest.cc
  AioImageRequestWQ.cc
  AioObjectRequest.cc
  AsyncObjectThrottle.cc
  AsyncOperation.cc
  AsyncRequest.cc
  CopyupRequest.cc
  DiffIterate.cc
  ExclusiveLock.cc
  ImageCtx.cc
  ImageState.cc
  ImageWatcher.cc
  internal.cc
  Journal.cc
  LibrbdAdminSocketHook.cc
  LibrbdWriteback.cc
  MirroringWatcher.cc
  ObjectMap.cc
  ObjectWatcher.cc
  Operations.cc
  Utils.cc
  exclusive_lock/AcquireRequest.cc
  exclusive_lock/ReleaseRequest.cc
  exclusive_lock/StandardPolicy.cc
  image/CloseRequest.cc
  image/OpenRequest.cc
  image/RefreshParentRequest.cc
  image/RefreshRequest.cc
  image/SetSnapRequest.cc
  image_watcher/Notifier.cc
  image_watcher/NotifyLockOwner.cc
  journal/Replay.cc
  journal/StandardPolicy.cc
  object_map/InvalidateRequest.cc
  object_map/LockRequest.cc
  object_map/Request.cc
  object_map/RefreshRequest.cc
  object_map/ResizeRequest.cc
  object_map/SnapshotCreateRequest.cc
  object_map/SnapshotRemoveRequest.cc
  object_map/SnapshotRollbackRequest.cc
  object_map/UnlockRequest.cc
  object_map/UpdateRequest.cc
  operation/FlattenRequest.cc
  operation/ObjectMapIterate.cc
  operation/RebuildObjectMapRequest.cc
  operation/RenameRequest.cc
  operation/Request.cc
  operation/ResizeRequest.cc
  operation/SnapshotCreateRequest.cc
  operation/SnapshotProtectRequest.cc
  operation/SnapshotRemoveRequest.cc
  operation/SnapshotRenameRequest.cc
  operation/SnapshotRollbackRequest.cc
  operation/SnapshotUnprotectRequest.cc
  operation/TrimRequest.cc)

add_library(rbd_api STATIC librbd.cc)
add_library(rbd_internal STATIC ${librbd_internal_srcs})
if(WITH_LTTNG)
  add_dependencies(rbd_internal rbd-tp)
endif()

set(librbd_srcs
  ${CMAKE_SOURCE_DIR}/src/common/ContextCompletion.cc
  librbd.cc)
add_library(librbd-objs OBJECT ${librbd_srcs})
set(librbd_objs
  $<TARGET_OBJECTS:librbd-objs>
  $<TARGET_OBJECTS:osdc_rbd_objs>
  $<TARGET_OBJECTS:common_util_obj>
  $<TARGET_OBJECTS:krbd_objs>
  $<TARGET_OBJECTS:parse_secret_objs>)
if(BUILD_SHARED_LIBS)
  list(APPEND targets librbd)
endif()
if(BUILD_STATIC_LIBS)
  list(APPEND targets librbd-static)
endif()

foreach(target ${targets})
  if(${target} MATCHES "-static$")
    add_library(${target} STATIC ${librbd_objs})
  else()
    add_library(${target} SHARED ${librbd_objs})
  endif()
  # LINK_PRIVATE instead of PRIVATE is used to backward compatibility with cmake 2.8.11
  target_link_libraries(${target} LINK_PRIVATE
    rbd_internal
    rbd_types
    journal
    librados
    common
    osdc
    cls_rbd_client
    cls_lock_client
    cls_journal_client
    pthread
    udev
    keyutils
    ${CMAKE_DL_LIBS}
    ${EXTRALIBS})
  set_target_properties(${target} PROPERTIES
      OUTPUT_NAME rbd
      VERSION 1.0.0
      SOVERSION 1)
  install(TARGETS ${target} DESTINATION ${CMAKE_INSTALL_LIBDIR})
endforeach(target ${targets})
if(NOT TARGET librbd)
  add_library(librbd ALIAS librbd-static)
endif()
