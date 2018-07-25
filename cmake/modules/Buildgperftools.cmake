if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gperftools/configure)
  set(gperftools_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gperftools)
  set(gperftools_SOURCE_DIR
    SOURCE_DIR ${gperftools_ROOT_DIR})
else()
  set(gperftools_ROOT_DIR ${CMAKE_CURRENT_BINARY_DIR}/gperftools)
  set(gperftools_SOURCE_DIR
    URL https://github.com/gperftools/gperftools/releases/download/gperftools-2.7/gperftools-2.7.tar.gz
    URL_HASH SHA256=1ee8c8699a0eff6b6a203e59b43330536b22bbcbe6448f54c7091e5efb0763c9)
endif()

include(ExternalProject)
ExternalProject_Add(gperftools_ext
  ${gperftools_SOURCE_DIR}
  PREFIX ${gperftools_ROOT_DIR}
  BUILD_IN_SOURCE TRUE
  CONFIGURE_COMMAND ./configure --disable-libunwind --disable-stacktrace-via-backtrace --enable-frame-pointers --prefix=<INSTALL_DIR> CXXFLAGS=-fPIC
  BUILD_COMMAND $(MAKE)
  INSTALL_COMMAND $(MAKE) install)

foreach(component tcmalloc tcmalloc_minimal profiler)
  add_library(gperftools::${component} STATIC IMPORTED)
  set_target_properties(gperftools::${component} PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${gperftools_ROOT_DIR}/include
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    IMPORTED_LOCATION ${gperftools_ROOT_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${component}${CMAKE_STATIC_LIBRARY_SUFFIX})
  add_dependencies(gperftools::${component} gperftools_ext)
endforeach()

find_package(Unwind REQUIRED)
find_package(Threads)
foreach(component tcmalloc profiler)
  set_target_properties(gperftools::${component} PROPERTIES
    INTERFACE_LINK_LIBRARIES "Threads::Threads")
endforeach()
