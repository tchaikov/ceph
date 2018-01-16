set(test_buffer_srcs
  test_buffer.cc
  $<TARGET_OBJECTS:seastar_buffer_obj>)
add_executable(unittest_seastar_buffer ${test_buffer_srcs})
add_ceph_unittest(unittest_seastar_buffer)
target_link_libraries(unittest_seastar_buffer common Seastar::Seastar)

add_executable(unittest_seastar_denc
  test_denc.cc
  $<TARGET_OBJECTS:seastar_buffer_obj>
  $<TARGET_OBJECTS:unit-main>)
target_include_directories(unittest_seastar_denc
  PUBLIC $<TARGET_PROPERTY:Seastar::Seastar,INTERFACE_INCLUDE_DIRECTORIES>)
add_ceph_unittest(unittest_seastar_denc)
target_link_libraries(unittest_seastar_denc common global Seastar::Seastar)
