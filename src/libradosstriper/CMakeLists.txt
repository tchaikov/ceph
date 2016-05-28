set(libradosstriper_srcs
  libradosstriper.cc
  RadosStriperImpl.cc
  MultiAioCompletionImpl.cc)
add_library(radosstriper-objs OBJECT ${libradosstriper_srcs})

if(BUILD_SHARED_LIBS)
  list(APPEND targets radosstriper)
endif()
if(BUILD_STATIC_LIBS)
  list(APPEND targets radosstriper-static)
endif()
foreach(target ${targets})
  if(${target} MATCHES "-static$")
    add_library(${target} STATIC $<TARGET_OBJECTS:radosstriper-objs>)
  else()
    add_library(${target} SHARED $<TARGET_OBJECTS:radosstriper-objs>)
  endif()
  target_link_libraries(${target} LINK_PRIVATE
    librados cls_lock_client osdc common pthread ${CRYPTO_LIBS} ${EXTRALIBS})
  set_target_properties(${target} PROPERTIES
      OUTPUT_NAME radosstriper
      VERSION 1.0.0
      SOVERSION 1)
  install(TARGETS ${target} DESTINATION ${CMAKE_INSTALL_LIBDIR})
endforeach(target ${targets})
if(NOT TARGET radosstriper)
  add_library(radosstriper ALIAS radosstriper-static)
endif()
