cmake_minimum_required(VERSION 3.24)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerCache)
pp_browser_configure_compiler_cache()

set(PP_BROWSER_VERSION "0.1.0" CACHE STRING "Numeric application version for CMake project()")
set(PP_BROWSER_RELEASE_VERSION "" CACHE STRING "Full release version (e.g. 0.1.0-rc1); defaults to PP_BROWSER_VERSION")
if(PP_BROWSER_RELEASE_VERSION STREQUAL "")
  set(PP_BROWSER_RELEASE_VERSION "${PP_BROWSER_VERSION}")
endif()

if(WIN32)
  # libp2p deps (Boost MASM, BoringSSL NASM) require these for Ninja + MSVC.
  project(pp-browser VERSION ${PP_BROWSER_VERSION} LANGUAGES CXX C ASM_MASM ASM_NASM)
elseif(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # Desktop macOS needs OBJCXX for .mm sources (e.g. LocalNotifier_Darwin.mm).
  # enable_language(OBJCXX) from a subdirectory leaves CMAKE_OBJCXX_COMPILE_OBJECT
  # unset at generate time under CMake 4.x.
  project(pp-browser VERSION ${PP_BROWSER_VERSION} LANGUAGES CXX C OBJCXX)
else()
  project(pp-browser VERSION ${PP_BROWSER_VERSION} LANGUAGES CXX C)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  # Keep vendored static libs and pp-browser on the MSVC default (/MD) CRT.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "MSVC runtime library" FORCE)

  # Prevent windows.h min/max macros from breaking std::min/std::max (soralog, etc.).
  add_compile_definitions(NOMINMAX)
  # Target Windows 7+; avoids Boost/winapi defaulting _WIN32_WINNT at compile time.
  add_compile_definitions(_WIN32_WINNT=0x0601)
endif()

pp_browser_finalize_compiler_cache()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(Platform)
include(PpBrowserWarnings)
include(PpBrowserLibp2p)
pp_browser_define_libp2p_options()

if(PP_BROWSER_IS_MOBILE)
  set(PP_BROWSER_BUILD_TESTS_DEFAULT OFF)
else()
  set(PP_BROWSER_BUILD_TESTS_DEFAULT ON)
endif()
option(PP_BROWSER_BUILD_TESTS "Build host unit and integration tests" ${PP_BROWSER_BUILD_TESTS_DEFAULT})

include(dependencies)
include(Progress)
find_package(Threads REQUIRED)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/render)
endif()
add_subdirectory(src/libp2p)

if(PP_BROWSER_BUILD_TESTS)
  pp_configure_status("Configuring tests...")
  include(PpBrowserTesting)
  enable_testing()
  pp_browser_enable_googletest()
endif()

add_subdirectory(src)
