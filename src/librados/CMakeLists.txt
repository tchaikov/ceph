add_library(librados_objs OBJECT
  IoCtxImpl.cc
  RadosXattrIter.cc
  RadosClient.cc)

# C API
add_library(rados_c_api OBJECT
  librados.cc)
target_compile_definitions(rados_c_api
  PRIVATE RADOS_C_API)
if(WITH_LTTNG)
  add_dependencies(rados_c_api librados-tp)
endif()
add_library(librados ${CEPH_SHARED}
  $<TARGET_OBJECTS:rados_c_api>
  $<TARGET_OBJECTS:librados_objs>
  $<TARGET_OBJECTS:common_buffer_obj>)
if(ENABLE_SHARED)
  set_target_properties(librados PROPERTIES
    OUTPUT_NAME rados
    VERSION 2.0.0
    SOVERSION 2
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  if(NOT APPLE)
    set_property(TARGET librados APPEND_STRING PROPERTY
      LINK_FLAGS " -Wl,--exclude-libs,ALL")
  endif()
  if(WITH_STATIC_LIBSTDCXX)
    set_property(TARGET librados APPEND_STRING PROPERTY
      LINK_FLAGS " -static-libstdc++ -static-libgcc")
  endif()
endif()

target_link_libraries(librados PRIVATE
  osdc ceph-common cls_lock_client
  ${BLKID_LIBRARIES} ${CRYPTO_LIBS} ${EXTRALIBS})
install(TARGETS librados DESTINATION ${CMAKE_INSTALL_LIBDIR})

# C++ API
add_library(rados_cxx_api OBJECT
  librados.cc)
target_compile_definitions(rados_cxx_api
  PRIVATE RADOS_CXX_API)
add_library(librados_cxx ${CEPH_SHARED}
  $<TARGET_OBJECTS:rados_cxx_api>)
if(ENABLE_SHARED)
  set_target_properties(librados_cxx PROPERTIES
    OUTPUT_NAME radospp
    VERSION 1.0.0
    SOVERSION 1
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  if(NOT APPLE)
    set_property(TARGET librados_cxx APPEND_STRING PROPERTY
      LINK_FLAGS " -Wl,--exclude-libs,ALL")
  endif()
endif(ENABLE_SHARED)
target_link_libraries(librados_cxx PRIVATE
  librados ceph-common cls_lock_client)
if(WITH_STATIC_LIBSTDCXX)
  target_link_libraries(librados_cxx
    INTERFACE "-static-libstdc++ -static-libgcc")
endif()
install(TARGETS librados_cxx DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(WITH_EMBEDDED)
  add_library(cephd_rados STATIC
    $<TARGET_OBJECTS:rados_c_api>
    $<TARGET_OBJECTS:rados_cxx_api>
    $<TARGET_OBJECTS:librados_objs>)
endif()
if(WITH_LTTNG AND WITH_EVENTTRACE)
  add_dependencies(librados_api_obj eventtrace_tp)
endif()
