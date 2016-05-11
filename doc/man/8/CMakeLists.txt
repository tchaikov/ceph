list(APPEND man8_srcs
  ceph-osd.rst
  ceph-mds.rst
  ceph-mon.rst
  ceph-syn.rst
  crushtool.rst
  osdmaptool.rst
  monmaptool.rst
  ceph-conf.rst
  ceph-run.rst
  ceph.rst
  mount.ceph.rst
  ceph-create-keys.rst
  radosgw.rst
  radosgw-admin.rst
  ceph-authtool.rst
  rados.rst
  librados-config.rst
  ceph-clsinfo.rst
  ceph-debugpack.rst
  cephfs.rst
  ceph-dencoder.rst
  ceph-rest-api.rst
  ceph-post-file.rst)

if(HAVE_LIBFUSE)
  list(APPEND man8_srcs
    ceph-fuse.rst
    rbd-fuse.rst)
endif()

if(WITH_RBD)
  list(APPEND man8_srcs
	ceph-rbdnamer.rst
	rbd-nbd.rst
	rbd-replay-prep.rst
	rbd-replay.rst
	rbdmap.rst
	rbd.rst)
endif()

foreach(manpage ${man8_srcs})
  list(APPEND man_srcs ${CMAKE_CURRENT_SOURCE_DIR}/${manpage})
  get_filename_component(cmd ${man} NAME_WE)
  list(APPEND man_output ${man_output_dir}/${cmd}.8)
endforeach()

install(FILES ${man_output}
  DESTINATION ${CEPH_MAN_DIR}/man8)
