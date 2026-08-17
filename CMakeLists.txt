cmake_minimum_required(VERSION 3.24)

list(APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
include(CompilerCache)
pp_browser_configure_compiler_cache()

set(PP_BROWSER_VERSION "0.1.0" CACHE STRING "Numeric application version for CMake project()")
set(PP_BROWSER_RELEASE_VERSION "" CACHE STRING "Full release version (e.g. 0.1.0-rc1); defaults to PP_BROWSER_VERSION")
if(PP_BROWSER_RELEASE_VERSION STREQUAL "")
  set(PP_BROWSER_RELEASE_VERSION "${PP_BROWSER_VERSION}")
endif()

# iOS deployment target must be set before project(): Ninja/clang use
# CMAKE_OSX_DEPLOYMENT_TARGET for -mios-version-min / LC_BUILD_VERSION.minos.
# XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET alone only affects -G Xcode.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(PP_BROWSER_IOS_DEPLOYMENT_TARGET "15.0" CACHE STRING
    "Minimum iOS version (IPHONEOS_DEPLOYMENT_TARGET / CMAKE_OSX_DEPLOYMENT_TARGET)")
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${PP_BROWSER_IOS_DEPLOYMENT_TARGET}" CACHE STRING
      "Minimum iOS version for linked binaries" FORCE)
  endif()
endif()

if(WIN32)
  # libp2p deps (Boost MASM, BoringSSL NASM) require these for Ninja + MSVC.
  # Generic ASM must also be declared here: vendored libpng does
  # project(... LANGUAGES C ASM) from a subdirectory, which leaves
  # CMAKE_ASM_COMPILE_OBJECT unset at generate time (same class of bug as OBJCXX).
  #
  # Do not let generic ASM claim `.asm`: CMake's extension→language map is
  # last-wins, and ASM defaults include `asm`. That steals BoringSSL NASM
  # sources so fipsmodule objects are never built and linking crypto.lib
  # fails with LNK1181 (missing *.asm.obj). libpng only needs `.S` on ARM.
  set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS "s;S")
  # ASM_NASM last so `.asm` maps to NASM until Boost re-enables ASM_MASM.
  project(pp-browser VERSION ${PP_BROWSER_VERSION} LANGUAGES CXX C ASM_MASM ASM ASM_NASM)
  if(NOT CMAKE_ASM_COMPILE_OBJECT)
    # There is no Compiler/MSVC-ASM.cmake; install a cl-compatible stub.
    # libpng's amd64 path uses C SSE2 intrinsics only (no .S sources).
    set(CMAKE_ASM_COMPILE_OBJECT
        "<CMAKE_C_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /c /Fo<OBJECT> <SOURCE>")
  endif()
  # CMP0091 applies CMAKE_MSVC_RUNTIME_LIBRARY to every language. Generic ASM
  # has no Compiler/MSVC-ASM.cmake mapping, so generate fails with
  # "MSVC_RUNTIME_LIBRARY value 'MultiThreadedDLL' not known for this ASM
  # compiler" (e.g. BoringSSL). Empty options match Boost's ARMASM stub.
  foreach(_rt MultiThreaded MultiThreadedDLL MultiThreadedDebug MultiThreadedDebugDLL)
    set(CMAKE_ASM_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_${_rt} "")
  endforeach()
  unset(_rt)
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
include(pp_lib_paths)
include(pp_lib_libp2p)
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

# Owned forks before layered src/ so base/feature can link them.
add_subdirectory(src/lib)

if(PP_BROWSER_BUILD_TESTS)
  pp_configure_status("Configuring tests...")
  include(PpBrowserTesting)
  enable_testing()
  pp_browser_enable_googletest()
endif()

add_subdirectory(src)
