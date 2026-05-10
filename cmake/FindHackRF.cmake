include(FindPackageHandleStandardArgs)

set(_HackRF_ROOT_HINTS)

foreach(_HackRF_ROOT_VAR IN ITEMS HackRF_ROOT HACKRF_ROOT)
  if(DEFINED ${_HackRF_ROOT_VAR} AND NOT "${${_HackRF_ROOT_VAR}}" STREQUAL "")
    list(APPEND _HackRF_ROOT_HINTS "${${_HackRF_ROOT_VAR}}")
  endif()
endforeach()

if(DEFINED ENV{HackRF_ROOT} AND NOT "$ENV{HackRF_ROOT}" STREQUAL "")
  list(APPEND _HackRF_ROOT_HINTS "$ENV{HackRF_ROOT}")
endif()

if(DEFINED ENV{HACKRF_ROOT} AND NOT "$ENV{HACKRF_ROOT}" STREQUAL "")
  list(APPEND _HackRF_ROOT_HINTS "$ENV{HACKRF_ROOT}")
endif()

find_path(
  HackRF_INCLUDE_DIR
  NAMES libhackrf/hackrf.h
  HINTS ${_HackRF_ROOT_HINTS}
  PATH_SUFFIXES include Include
)

find_library(
  HackRF_LIBRARY
  NAMES hackrf libhackrf
  HINTS ${_HackRF_ROOT_HINTS}
  PATH_SUFFIXES lib lib64 Lib Lib64 bin Bin
)

find_file(
  HackRF_RUNTIME_LIBRARY
  NAMES hackrf.dll libhackrf.dll
  HINTS ${_HackRF_ROOT_HINTS}
  PATH_SUFFIXES bin Bin lib lib64 Lib Lib64
)

find_package_handle_standard_args(
  HackRF
  REQUIRED_VARS HackRF_LIBRARY HackRF_INCLUDE_DIR
)

if(HackRF_FOUND AND NOT TARGET HackRF::HackRF)
  if(WIN32 AND HackRF_RUNTIME_LIBRARY)
    add_library(HackRF::HackRF SHARED IMPORTED)
    set_target_properties(
      HackRF::HackRF
      PROPERTIES
        IMPORTED_IMPLIB "${HackRF_LIBRARY}"
        IMPORTED_LOCATION "${HackRF_RUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HackRF_INCLUDE_DIR}"
    )
  else()
    add_library(HackRF::HackRF UNKNOWN IMPORTED)
    set_target_properties(
      HackRF::HackRF
      PROPERTIES
        IMPORTED_LOCATION "${HackRF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HackRF_INCLUDE_DIR}"
    )
  endif()
endif()

set(HACKRF_INCLUDE_DIR "${HackRF_INCLUDE_DIR}")
set(HACKRF_INCLUDE_DIRS "${HackRF_INCLUDE_DIR}")
set(HACKRF_LIBRARY "${HackRF_LIBRARY}")
set(HACKRF_LIBRARIES "${HackRF_LIBRARY}")
set(HACKRF_RUNTIME_LIBRARY "${HackRF_RUNTIME_LIBRARY}")

mark_as_advanced(
  HackRF_INCLUDE_DIR
  HackRF_LIBRARY
  HackRF_RUNTIME_LIBRARY
)

unset(_HackRF_ROOT_HINTS)
