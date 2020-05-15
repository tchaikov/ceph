add_executable(unittest_transaction_manager
  test_transaction_manager.cc
  ../gtest_seastar.cc)
add_ceph_unittest(unittest_transaction_manager)
target_link_libraries(
  unittest_transaction_manager
  ${CMAKE_DL_LIBS}
  crimson-seastore)

add_executable(unittest_lba_btree_node
  test_lba_btree_node.cc
  ../gtest_seastar.cc)
add_ceph_unittest(unittest_lba_btree_node)
target_link_libraries(
  unittest_lba_btree_node
  ${CMAKE_DL_LIBS}
  crimson-seastore
  GTest::Main)

add_executable(unittest_btree_lba_manager
  test_btree_lba_manager.cc
  ../gtest_seastar.cc)
add_ceph_unittest(unittest_btree_lba_manager)
target_link_libraries(
  unittest_btree_lba_manager
  ${CMAKE_DL_LIBS}
  crimson-seastore)

add_executable(unittest_seastore_journal
  test_seastore_journal.cc
  ../gtest_seastar.cc)
add_ceph_unittest(unittest_seastore_journal)
target_link_libraries(
  unittest_seastore_journal
  ${CMAKE_DL_LIBS}
  crimson-seastore)

add_executable(unittest_seastore_cache
  test_seastore_cache.cc
  ../gtest_seastar.cc)
add_ceph_unittest(unittest_seastore_cache)
target_link_libraries(
  unittest_seastore_cache
  ${CMAKE_DL_LIBS}
  crimson-seastore)
