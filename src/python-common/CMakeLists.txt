include(Distutils)
distutils_install_module(ceph
  PYTHON_VERSION 3)

if(WITH_TESTS)
  include(AddCephTest)
  add_tox_test(python-common TOX_ENVS lint)
endif()
