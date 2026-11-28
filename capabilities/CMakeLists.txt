# The capabilities on their own, without the app (projects/capability-repos CR023 step 4): a configure of chosen
# capability folders and what is below them, with the pin list's libraries (cmake/PpPins.cmake) and third_party/ —
# what a capability repo's root will be (CR027). pp-browser's own configure does not read this file: the app adds
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

set(PP_CAPABILITIES_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PP_CAPABILITIES_DEFAULT "calls")
include(PpCapabilitiesRoot)
