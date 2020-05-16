add_executable(test-seastore-onode-tree-node
  test_node.cc)
add_ceph_unittest(test-seastore-onode-tree-node)
target_link_libraries(test-seastore-onode-tree-node
  crimson-seastore
  GTest::Main)

add_executable(unittest-seastore-onode-tree-tree
  test_tree.cc)
add_ceph_test(unittest-seastore-onode-tree-tree
  unittest-seastore-onode-tree-tree)
target_link_libraries(
  unittest-seastore-onode-tree-tree
  crimson-seastore
  crimson::gtest)
