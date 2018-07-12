# Try to find gperftools
# Once done, this will define
#
# GPERFTOOLS_FOUND - system has Profiler
# GPERFTOOLS_INCLUDE_DIR - the Profiler include directories
# Tcmalloc_INCLUDE_DIR - where to find Tcmalloc.h
# GPERFTOOLS_TCMALLOC_LIBRARY - link it to use tcmalloc
# GPERFTOOLS_TCMALLOC_MINIMAL_LIBRARY - link it to use tcmalloc_minimal
# GPERFTOOLS_PROFILER_LIBRARY - link it to use Profiler
# TCMALLOC_VERSION_STRING
# TCMALLOC_VERSION_MAJOR
# TCMALLOC_VERSION_MINOR
# TCMALLOC_VERSION_PATCH

find_path(GPERFTOOLS_INCLUDE_DIR gperftools/profiler.h
  HINTS $ENV{GPERF_ROOT}/include)
find_path(Tcmalloc_INCLUDE_DIR gperftools/tcmalloc.h
  HINTS $ENV{GPERF_ROOT}/include)

if(Tcmalloc_INCLUDE_DIR AND EXISTS "${Tcmalloc_INCLUDE_DIR}/gperftools/tcmalloc.h")
  foreach(ver "MAJOR" "MINOR" "PATCH")
    file(STRINGS "${Tcmalloc_INCLUDE_DIR}/gperftools/tcmalloc.h" TC_VER_${ver}_LINE
      REGEX "^#define[ \t]+TC_VERSION_${ver}[ \t]+[^ \t]+$")
    string(REGEX REPLACE "^#define[ \t]+TC_VERSION_${ver}[ \t]+(\".)?([0-9]*)\"?$"
      "\\2" TCMALLOC_VERSION_${ver} "${TC_VER_${ver}_LINE}")
    unset(TC_VER_${ver}_LINE)
  endforeach()
  set(TCMALLOC_VERSION_STRING "${TCMALLOC_VERSION_MAJOR}.${TCMALLOC_VERSION_MINOR}")
  if(NOT TCMALLOC_VERSION_PATCH STREQUAL "")
    set(TCMALLOC_VERSION_STRING "${TCMALLOC_VERSION_STRING}.${TCMALLOC_VERSION_PATCH}")
  endif()
endif()

if(GPERFTOOLS_USE_STATIC_LIBS)
  find_package(Unwind REQUIRED)
  find_package(Threads)
  set(_gperftools_orig_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
endif()

foreach(component tcmalloc tcmalloc_minimal profiler)
  string(TOUPPER ${component} COMPONENT)
  find_library(GPERFTOOLS_${COMPONENT}_LIBRARY ${component}
    HINTS $ENV{GPERF_ROOT}/lib)
  list(APPEND GPERFTOOLS_LIBRARIES GPERFTOOLS_${COMPONENT}_LIBRARY)
  add_library(gperftools::${component} UNKNOWN IMPORTED)
  set_target_properties(gperftools::${component} PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${GPERFTOOLS_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    IMPORTED_LOCATION "${GPERFTOOLS_${COMPONENT}_LIBRARY}")
  if(GPERFTOOLS_USE_STATIC_LIBS)
    if(NOT component STREQUAL tcmalloc_minimal)
      set_target_properties(gperftools::${component} PROPERTIES
        INTERFACE_LINK_LIBRARIES "Unwind::unwind;Threads::Threads")
    endif()
  endif()
endforeach()

if(GPERFTOOLS_USE_STATIC_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_gperftools_orig_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(gperftools
  REQUIRED_VARS ${GPERFTOOLS_LIBRARIES} GPERFTOOLS_INCLUDE_DIR
  VERSION_VAR TCMALLOC_VERSION_STRING)

mark_as_advanced(${GPERFTOOLS_LIBRARIES} GPERFTOOLS_INCLUDE_DIR)
