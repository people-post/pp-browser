# The capabilities on their own, without the app (projects/capability-repos CR023 step 4): a configure of chosen
# capability folders and what is below them, with the pin list's libraries (cmake/PpPins.cmake) and third_party/ —
# what the capabilities repo's root will be (CR006). pp-browser's own configure does not read this file: the app adds
# each folder from src/CMakeLists.txt.
#
#   cmake -S capabilities -B build-calls -DPP_CAPABILITIES=calls
#   cmake --build build-calls --target pp_capability_calls_tests   # calls' tests (and what they link); or everything
#   ctest --test-dir build-calls -L '^capability:calls$'
#
# Linux (CI's capabilities job) and macOS desktop. Windows and the phones build capabilities through the app.
cmake_minimum_required(VERSION 3.24)

get_filename_component(_pp_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${_pp_root}/cmake")
include(CompilerCache)
pp_browser_configure_compiler_cache()

if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES "iOS|Android")
  set(PP_BROWSER_MACOS_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version (CMAKE_OSX_DEPLOYMENT_TARGET)")
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${PP_BROWSER_MACOS_DEPLOYMENT_TARGET}" CACHE STRING
      "Minimum macOS version for linked binaries" FORCE)
  endif()
  project(pp-capabilities LANGUAGES CXX C OBJCXX)
else()
  project(pp-capabilities LANGUAGES CXX C)
endif()
if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "iOS|Android")
  message(FATAL_ERROR "A standalone capabilities build is for Linux and macOS: build ${CMAKE_SYSTEM_NAME} "
                      "through the app")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
pp_browser_finalize_compiler_cache()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# What each capability builds on: the capability order (DESIGN § Target). check_capability_edges.py holds this table
# to its own (ALLOWED), so the two cannot drift.
set(PP_CAPABILITY_BELOW_base "")
set(PP_CAPABILITY_BELOW_people base)
set(PP_CAPABILITY_BELOW_net base people)
set(PP_CAPABILITY_BELOW_social base people)
set(PP_CAPABILITY_BELOW_delivery base people net)
set(PP_CAPABILITY_BELOW_pricing base people net delivery)
set(PP_CAPABILITY_BELOW_media base)
set(PP_CAPABILITY_BELOW_messaging base people net social delivery pricing media)
set(PP_CAPABILITY_BELOW_calls base people net delivery pricing media)
set(PP_CAPABILITY_BELOW_broadcast base people net delivery pricing media calls)
set(PP_CAPABILITY_BELOW_node base people net)
set(PP_CAPABILITY_BELOW_relay base people net node)
# The folders in the order a capability's targets need the ones below it defined first (src/CMakeLists.txt's order).
set(_pp_order base people net social delivery pricing media messaging calls broadcast node relay)

set(PP_CAPABILITIES "calls" CACHE STRING "The capabilities to build, with everything below them (;-list)")
set(_pp_built "")
foreach(_pp_cap IN LISTS PP_CAPABILITIES)
  if(NOT _pp_cap IN_LIST _pp_order)
    message(FATAL_ERROR "PP_CAPABILITIES: no capability '${_pp_cap}' (capabilities: ${_pp_order})")
  endif()
  list(APPEND _pp_built ${_pp_cap} ${PP_CAPABILITY_BELOW_${_pp_cap}})
endforeach()

include(Platform)
include(PpBrowserWarnings)
include(PpBrowserCapability)
include(PpPins)
include(PpCppCommon)
include(PpCppCrypto)
include(PpCppAmp)
include(PpCppMesh)
if("node" IN_LIST _pp_built)
  include(PpLedger)
endif()

option(PP_BROWSER_BUILD_TESTS "Build the capabilities' unit and in-process tests" ON)
include(dependencies)
if(NOT PP_BROWSER_HEADLESS)
  include(PpCppUi) # SDL3 for media's devices
endif()
find_package(Threads REQUIRED)
if(PP_BROWSER_BUILD_TESTS)
  include(PpBrowserTesting)
  enable_testing()
  pp_browser_enable_googletest()
endif()

set(_pp_folders "")
foreach(_pp_cap IN LISTS _pp_order)
  if(_pp_cap IN_LIST _pp_built)
    # The app's binary layout (build/capabilities/<name>): a compiler cache the app's build filled serves this one.
    add_subdirectory(${_pp_cap} ${CMAKE_BINARY_DIR}/capabilities/${_pp_cap})
    list(APPEND _pp_folders ${_pp_cap})
  endif()
endforeach()
message(STATUS "pp-capabilities: ${PP_CAPABILITIES} — building ${_pp_folders}")

# Nothing above the chosen capabilities is here, so a target that links a pp_* name this configure does not define
# links something above it (or a typo): fail now, not as a missing -l at link time.
function(pp_capabilities_check_links dir)
  get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_t IN LISTS _targets)
    get_target_property(_type ${_t} TYPE)
    set(_libs "")
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
      get_target_property(_direct ${_t} LINK_LIBRARIES)
      if(_direct)
        list(APPEND _libs ${_direct})
      endif()
    endif()
    get_target_property(_iface ${_t} INTERFACE_LINK_LIBRARIES)
    if(_iface)
      list(APPEND _libs ${_iface})
    endif()
    foreach(_l IN LISTS _libs)
      if(_l MATCHES "^pp_" AND NOT TARGET ${_l})
        message(FATAL_ERROR "${_t} links ${_l}, which is not in this build: it is above ${PP_CAPABILITIES}, "
                            "or not a target (projects/capability-repos DESIGN § Target)")
      endif()
    endforeach()
  endforeach()
  get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_d IN LISTS _subdirs)
    pp_capabilities_check_links("${_d}")
  endforeach()
endfunction()
foreach(_pp_cap IN LISTS _pp_folders)
  pp_capabilities_check_links("${CMAKE_CURRENT_SOURCE_DIR}/${_pp_cap}")
endforeach()

# pp_capability_<name>_tests: one capability's test executables (and so its targets), without the rest.
if(PP_BROWSER_BUILD_TESTS)
  foreach(_pp_cap IN LISTS _pp_folders)
    get_property(_pp_tests GLOBAL PROPERTY PP_BROWSER_CAPABILITY_TESTS_${_pp_cap})
    add_custom_target(pp_capability_${_pp_cap}_tests)
    if(_pp_tests)
      add_dependencies(pp_capability_${_pp_cap}_tests ${_pp_tests})
    endif()
  endforeach()
endif()
