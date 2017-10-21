set(crimson_net_srcs
  Errors.cc
  SocketConnection.cc
  SocketMessenger.cc)
add_library(crimson_net_objs OBJECT ${crimson_net_srcs})
set_target_properties(crimson_net_objs PROPERTIES CXX_STANDARD 14)
target_compile_definitions(crimson_net_objs
  PUBLIC $<TARGET_PROPERTY:Seastar::Seastar,INTERFACE_COMPILE_DEFINITIONS>)
target_include_directories(crimson_net_objs
  PUBLIC $<TARGET_PROPERTY:Seastar::Seastar,INTERFACE_INCLUDE_DIRECTORIES>)
