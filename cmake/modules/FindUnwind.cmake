find_path(UNWIND_INCLUDE_DIR NAMES libunwind.h)

find_library(UNWIND_LIBRARY NAMES unwind)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(UNWIND DEFAULT_MSG UNWIND_LIBRARY UNWIND_INCLUDE_DIR)

add_library(Unwind::unwind UNKNOWN IMPORTED)
set_target_properties(Unwind::unwind PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${UNWIND_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${UNWIND_LIBRARY}")

mark_as_advanced(UNWIND_INCLUDE_DIR UNWIND_LIBRARY)
