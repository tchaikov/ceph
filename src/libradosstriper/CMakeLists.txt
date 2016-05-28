set(libradosstriper_srcs
  libradosstriper.cc
  RadosStriperImpl.cc
  MultiAioCompletionImpl.cc)
include(AddCephLibrary)
ceph_add_library(radosstriper ${libradosstriper_srcs})
ceph_link_libraries(radosstriper librados cls_lock_client osdc common pthread ${CRYPTO_LIBS} ${EXTRALIBS})
set_target_properties(radosstriper PROPERTIES
  OUPUT_NAME radosstriper
  VERSION "1.0.0"
  SOVERSION "1")
ceph_install_library(radosstriper)
