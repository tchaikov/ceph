add_executable(ceph_test_rados_cls_test_remote_reads
  test_cls_test_remote_reads.cc
  )
target_link_libraries(ceph_test_rados_cls_test_remote_reads
  librados
  global
  ${EXTRALIBS}
  ${BLKID_LIBRARIES}
  ${CMAKE_DL_LIBS}
  radostest-cxx
  ${UNITTEST_LIBS}
  )
install(TARGETS
  ceph_test_rados_cls_test_remote_reads
  DESTINATION ${CMAKE_INSTALL_BINDIR})
