cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SSRX_VERSION)
string(STRIP "${SSRX_VERSION}" SSRX_VERSION)

project(ssrx VERSION ${SSRX_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SSRX_BUILD_HACKRF "Build ssrx-hackrf" ON)
option(SSRX_BUILD_RBSTAT "Build ssrx-rbstat" ON)
option(SSRX_BUILD_USRP "Build ssrx-usrp" OFF)

set(SSRX_INSTALL_TARGETS)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Winit-self -Wno-unused-parameter -Wfloat-equal)
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
  set(SSRX_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  set(SSRX_SQLITE_TARGET SQLite::SQLite3)
else()
  add_library(ssrx_sqlite INTERFACE)
  target_include_directories(ssrx_sqlite INTERFACE ${SQLite3_INCLUDE_DIRS})
  target_link_libraries(ssrx_sqlite INTERFACE ${SQLite3_LIBRARIES})
  set(SSRX_SQLITE_TARGET ssrx_sqlite)
endif()

find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
  find_package(yaml-cpp REQUIRED)
endif()
if(TARGET yaml-cpp::yaml-cpp)
  set(SSRX_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
  set(SSRX_YAML_CPP_TARGET yaml-cpp)
endif()

find_package(PkgConfig QUIET)

set(SSRX_CURSES_FOUND OFF)
set(SSRX_CURSES_TARGET "")
if(WIN32)
  find_package(unofficial-pdcurses CONFIG QUIET)
  if(TARGET unofficial::pdcurses::pdcurses)
    add_library(ssrx_curses INTERFACE)
    target_link_libraries(ssrx_curses INTERFACE unofficial::pdcurses::pdcurses)
    set(SSRX_CURSES_FOUND ON)
    set(SSRX_CURSES_TARGET ssrx_curses)
    find_file(SSRX_PDCURSES_RUNTIME_LIBRARY NAMES pdcurses.dll PATH_SUFFIXES bin)
  endif()
else()
  find_package(Curses QUIET)
  if(CURSES_FOUND)
    add_library(ssrx_curses INTERFACE)
    target_include_directories(ssrx_curses INTERFACE ${CURSES_INCLUDE_DIRS})
    target_compile_options(ssrx_curses INTERFACE ${CURSES_CFLAGS})
    target_link_libraries(ssrx_curses INTERFACE ${CURSES_LIBRARIES})
    set(SSRX_CURSES_FOUND ON)
    set(SSRX_CURSES_TARGET ssrx_curses)
  endif()
endif()

set(SSRX_ZMQ_LINK_TARGETS "")
find_package(cppzmq CONFIG QUIET)
find_package(ZeroMQ CONFIG QUIET)
if(TARGET cppzmq)
  list(APPEND SSRX_ZMQ_LINK_TARGETS cppzmq)
endif()
if(TARGET libzmq)
  list(APPEND SSRX_ZMQ_LINK_TARGETS libzmq)
elseif(TARGET ZeroMQ::ZeroMQ)
  list(APPEND SSRX_ZMQ_LINK_TARGETS ZeroMQ::ZeroMQ)
endif()
if(NOT SSRX_ZMQ_LINK_TARGETS)
  if(PkgConfig_FOUND)
    pkg_check_modules(ZMQ REQUIRED libzmq)
    add_library(ssrx_zmq INTERFACE)
    target_include_directories(ssrx_zmq INTERFACE ${ZMQ_INCLUDE_DIRS})
    target_link_directories(ssrx_zmq INTERFACE ${ZMQ_LIBRARY_DIRS})
    target_link_libraries(ssrx_zmq INTERFACE ${ZMQ_LIBRARIES})
    list(APPEND SSRX_ZMQ_LINK_TARGETS ssrx_zmq)
  else()
    message(FATAL_ERROR "ZeroMQ not found. Install cppzmq/libzmq or enable pkg-config.")
  endif()
endif()

set(SSRX_FFTW3F_LINK_TARGET "")
if(PkgConfig_FOUND)
  pkg_check_modules(FFTW3F QUIET fftw3f)
endif()
if(FFTW3F_FOUND)
  add_library(ssrx_fftw3f INTERFACE)
  target_include_directories(ssrx_fftw3f INTERFACE ${FFTW3F_INCLUDE_DIRS})
  target_link_directories(ssrx_fftw3f INTERFACE ${FFTW3F_LIBRARY_DIRS})
  target_link_libraries(ssrx_fftw3f INTERFACE ${FFTW3F_LIBRARIES})
  set(SSRX_FFTW3F_LINK_TARGET ssrx_fftw3f)
else()
  find_path(FFTW3F_INCLUDE_DIR NAMES fftw3.h)
  find_library(FFTW3F_LIB NAMES fftw3f)
  if(NOT FFTW3F_INCLUDE_DIR OR NOT FFTW3F_LIB)
    message(FATAL_ERROR "fftw3f not found.")
  endif()
  add_library(ssrx_fftw3f INTERFACE)
  target_include_directories(ssrx_fftw3f INTERFACE ${FFTW3F_INCLUDE_DIR})
  target_link_libraries(ssrx_fftw3f INTERFACE ${FFTW3F_LIB})
  set(SSRX_FFTW3F_LINK_TARGET ssrx_fftw3f)
endif()

function(ssrx_apply_common_settings target_name)
  target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${target_name} PRIVATE Threads::Threads)
  target_compile_definitions(${target_name} PRIVATE
    VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    VERSION_MINOR=${PROJECT_VERSION_MINOR}
    VERSION_PATCH=${PROJECT_VERSION_PATCH}
  )
endfunction()

function(ssrx_require_cli11 target_name)
  if(NOT TARGET CLI11::CLI11)
    set(SSRX_CLI11_PREFIX_HINTS
      /opt/homebrew/opt/cli11
      /usr/local/opt/cli11
      /opt/homebrew
      /usr/local
    )
    find_package(CLI11 CONFIG QUIET HINTS ${SSRX_CLI11_PREFIX_HINTS})
    if(NOT TARGET CLI11::CLI11)
      find_path(CLI11_INCLUDE_DIR
        NAMES CLI/CLI.hpp
        HINTS
          /opt/homebrew/opt/cli11/include
          /usr/local/opt/cli11/include
          /opt/homebrew/include
          /usr/local/include
      )
      if(NOT CLI11_INCLUDE_DIR)
        message(FATAL_ERROR "CLI11 not found. Install libcli11-dev, brew install cli11, or use vcpkg manifest mode.")
      endif()
      add_library(CLI11::CLI11 INTERFACE IMPORTED)
      set_target_properties(CLI11::CLI11 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CLI11_INCLUDE_DIR}"
      )
    endif()
  endif()
  target_link_libraries(${target_name} PRIVATE CLI11::CLI11)
endfunction()

set(SSRX_UHD_PREFIX_HINTS)
function(ssrx_append_uhd_prefix_hint hint_value)
  if(hint_value AND NOT hint_value MATCHES "-NOTFOUND$")
    file(TO_CMAKE_PATH "${hint_value}" _ssrx_uhd_hint_path)
    list(APPEND SSRX_UHD_PREFIX_HINTS "${_ssrx_uhd_hint_path}")
    set(SSRX_UHD_PREFIX_HINTS "${SSRX_UHD_PREFIX_HINTS}" PARENT_SCOPE)
  endif()
endfunction()

foreach(_ssrx_uhd_hint IN ITEMS UHD_ROOT UHD_DIR)
  if(DEFINED ${_ssrx_uhd_hint})
    ssrx_append_uhd_prefix_hint("${${_ssrx_uhd_hint}}")
  endif()
  if(DEFINED ENV{${_ssrx_uhd_hint}})
    ssrx_append_uhd_prefix_hint("$ENV{${_ssrx_uhd_hint}}")
  endif()
endforeach()

if(SSRX_BUILD_USRP)
  find_package(UHD CONFIG QUIET HINTS ${SSRX_UHD_PREFIX_HINTS})
  if(NOT UHD_FOUND)
    find_package(UHD MODULE QUIET)
  endif()
  if(UHD_FOUND)
    add_library(ssrx_uhd INTERFACE)
    if(TARGET UHD::uhd)
      target_link_libraries(ssrx_uhd INTERFACE UHD::uhd)
    elseif(TARGET uhd::uhd)
      target_link_libraries(ssrx_uhd INTERFACE uhd::uhd)
    elseif(TARGET UHD::UHD)
      target_link_libraries(ssrx_uhd INTERFACE UHD::UHD)
    else()
      target_include_directories(ssrx_uhd INTERFACE ${UHD_INCLUDE_DIRS})
      target_link_libraries(ssrx_uhd INTERFACE ${UHD_LIBRARIES})
    endif()

    if(WIN32)
      find_file(SSRX_UHD_RUNTIME_LIBRARY
        NAMES uhd.dll libuhd.dll
        HINTS ${SSRX_UHD_PREFIX_HINTS}
        PATH_SUFFIXES bin Bin lib Lib
      )
    endif()

    add_executable(ssrx-usrp src/ssrx-usrp.cpp src/modes.cpp src/algo.cpp)
    ssrx_apply_common_settings(ssrx-usrp)
    ssrx_require_cli11(ssrx-usrp)
    target_link_libraries(
      ssrx-usrp
      PRIVATE
      ${SSRX_ZMQ_LINK_TARGETS}
      ${SSRX_FFTW3F_LINK_TARGET}
      ${SSRX_SQLITE_TARGET}
      ${SSRX_YAML_CPP_TARGET}
      ssrx_uhd
    )
    if(SSRX_CURSES_FOUND)
      target_link_libraries(ssrx-usrp PRIVATE ${SSRX_CURSES_TARGET})
    endif()
    if(WIN32 AND SSRX_UHD_RUNTIME_LIBRARY)
      add_custom_command(
        TARGET ssrx-usrp
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SSRX_UHD_RUNTIME_LIBRARY}"
          "$<TARGET_FILE_DIR:ssrx-usrp>"
      )
    endif()
    list(APPEND SSRX_INSTALL_TARGETS ssrx-usrp)
  else()
    message(WARNING "UHD library not found; skipping ssrx-usrp. Set -DSSRX_BUILD_USRP=OFF to silence this warning.")
  endif()
endif()

if(SSRX_BUILD_RBSTAT)
  if(SSRX_CURSES_FOUND)
    add_executable(ssrx-rbstat src/ssrx-rbstat.cpp)
    ssrx_apply_common_settings(ssrx-rbstat)
    ssrx_require_cli11(ssrx-rbstat)
    target_link_libraries(ssrx-rbstat PRIVATE ${SSRX_ZMQ_LINK_TARGETS} ${SSRX_CURSES_TARGET} ${SSRX_YAML_CPP_TARGET})
    list(APPEND SSRX_INSTALL_TARGETS ssrx-rbstat)
  else()
    message(WARNING "Curses/PDCurses not found; skipping ssrx-rbstat. Set -DSSRX_BUILD_RBSTAT=OFF to silence this warning.")
  endif()
endif()

if(SSRX_BUILD_HACKRF)
  find_package(HackRF MODULE QUIET)
  if(HackRF_FOUND)
    add_executable(ssrx-hackrf src/ssrx-hackrf.cpp src/modes.cpp src/algo.cpp)
    ssrx_apply_common_settings(ssrx-hackrf)
    ssrx_require_cli11(ssrx-hackrf)
    target_link_libraries(
      ssrx-hackrf
      PRIVATE
      ${SSRX_ZMQ_LINK_TARGETS}
      ${SSRX_FFTW3F_LINK_TARGET}
      ${SSRX_SQLITE_TARGET}
      ${SSRX_YAML_CPP_TARGET}
      HackRF::HackRF
    )
    if(SSRX_CURSES_FOUND)
      target_link_libraries(ssrx-hackrf PRIVATE ${SSRX_CURSES_TARGET})
    endif()
    if(WIN32 AND HackRF_RUNTIME_LIBRARY)
      add_custom_command(
        TARGET ssrx-hackrf
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${HackRF_RUNTIME_LIBRARY}"
          "$<TARGET_FILE_DIR:ssrx-hackrf>"
      )
    endif()
    list(APPEND SSRX_INSTALL_TARGETS ssrx-hackrf)
  else()
    message(WARNING "HackRF library not found; skipping ssrx-hackrf. Set -DSSRX_BUILD_HACKRF=OFF to silence this warning.")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(ssrx-test-modes tests/cpp/test_modes.cpp src/modes.cpp)
  ssrx_apply_common_settings(ssrx-test-modes)
  target_link_libraries(ssrx-test-modes PRIVATE ${SSRX_FFTW3F_LINK_TARGET} ${SSRX_SQLITE_TARGET})
  add_test(NAME ssrx-test-modes COMMAND ssrx-test-modes)

  add_executable(ssrx-test-confighelper tests/cpp/test_confighelper.cpp)
  ssrx_apply_common_settings(ssrx-test-confighelper)
  target_link_libraries(ssrx-test-confighelper PRIVATE ${SSRX_YAML_CPP_TARGET})
  add_test(NAME ssrx-test-confighelper COMMAND ssrx-test-confighelper)

  add_executable(ssrx-test-mqhelper tests/cpp/test_mqhelper.cpp)
  ssrx_apply_common_settings(ssrx-test-mqhelper)
  target_link_libraries(ssrx-test-mqhelper PRIVATE ${SSRX_ZMQ_LINK_TARGETS} ${SSRX_YAML_CPP_TARGET})
  add_test(NAME ssrx-test-mqhelper COMMAND ssrx-test-mqhelper)
endif()

if(SSRX_INSTALL_TARGETS)
  install(
    TARGETS ${SSRX_INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

if(WIN32 AND SSRX_BUILD_HACKRF AND HackRF_FOUND AND HackRF_RUNTIME_LIBRARY)
  install(
    FILES "${HackRF_RUNTIME_LIBRARY}"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

if(WIN32 AND SSRX_CURSES_FOUND AND SSRX_PDCURSES_RUNTIME_LIBRARY)
  install(
    FILES "${SSRX_PDCURSES_RUNTIME_LIBRARY}"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

if(WIN32 AND SSRX_BUILD_USRP AND UHD_FOUND AND SSRX_UHD_RUNTIME_LIBRARY)
  install(
    FILES "${SSRX_UHD_RUNTIME_LIBRARY}"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()
