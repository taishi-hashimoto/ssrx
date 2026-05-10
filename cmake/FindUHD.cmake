include(FindPackageHandleStandardArgs)

set(_UHD_HINTS)
function(_uhd_append_hint hint_value)
  if(hint_value AND NOT hint_value MATCHES "-NOTFOUND$")
    file(TO_CMAKE_PATH "${hint_value}" _uhd_hint_path)
    list(APPEND _UHD_HINTS "${_uhd_hint_path}")
    set(_UHD_HINTS "${_UHD_HINTS}" PARENT_SCOPE)
  endif()
endfunction()

foreach(_uhd_hint IN ITEMS UHD_ROOT UHD_DIR)
  if(DEFINED ${_uhd_hint})
    _uhd_append_hint("${${_uhd_hint}}")
  endif()
  if(DEFINED ENV{${_uhd_hint}})
    _uhd_append_hint("$ENV{${_uhd_hint}}")
  endif()
endforeach()

find_path(UHD_INCLUDE_DIR
  NAMES uhd/usrp/multi_usrp.hpp
  HINTS ${_UHD_HINTS}
  PATH_SUFFIXES include Include
)

find_library(UHD_LIBRARY
  NAMES uhd libuhd
  HINTS ${_UHD_HINTS}
  PATH_SUFFIXES lib lib64 Lib Lib64 bin Bin
)

find_file(UHD_RUNTIME_LIBRARY
  NAMES uhd.dll libuhd.dll
  HINTS ${_UHD_HINTS}
  PATH_SUFFIXES bin Bin lib Lib
)

find_package_handle_standard_args(UHD
  REQUIRED_VARS UHD_LIBRARY UHD_INCLUDE_DIR
)

if(UHD_FOUND)
  set(UHD_INCLUDE_DIRS "${UHD_INCLUDE_DIR}")
  set(UHD_LIBRARIES "${UHD_LIBRARY}")
endif()

if(UHD_FOUND AND NOT TARGET UHD::UHD)
  if(WIN32 AND UHD_RUNTIME_LIBRARY)
    add_library(UHD::UHD SHARED IMPORTED)
    set_target_properties(UHD::UHD PROPERTIES
      IMPORTED_IMPLIB "${UHD_LIBRARY}"
      IMPORTED_LOCATION "${UHD_RUNTIME_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${UHD_INCLUDE_DIR}"
    )
  else()
    add_library(UHD::UHD UNKNOWN IMPORTED)
    set_target_properties(UHD::UHD PROPERTIES
      IMPORTED_LOCATION "${UHD_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${UHD_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(UHD_INCLUDE_DIR UHD_LIBRARY UHD_RUNTIME_LIBRARY)
