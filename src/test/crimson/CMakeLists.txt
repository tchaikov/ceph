set(test_buffer_srcs
  test_buffer.cc
  $<TARGET_OBJECTS:seastar_buffer_obj>)
add_executable(unittest_seastar_buffer ${test_buffer_srcs})
add_ceph_unittest(unittest_seastar_buffer)
set_target_properties(unittest_seastar_buffer PROPERTIES CXX_STANDARD 14)
target_link_libraries(unittest_seastar_buffer common Seastar::Seastar)
