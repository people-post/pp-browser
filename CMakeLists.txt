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

if(PP_BROWSER_IS_MOBILE)
  set(PP_BROWSER_LIBP2P_TESTING_DEFAULT OFF)
else()
  set(PP_BROWSER_LIBP2P_TESTING_DEFAULT ON)
endif()
option(PP_BROWSER_LIBP2P_TESTING "Build in-tree libp2p unit tests" ${PP_BROWSER_LIBP2P_TESTING_DEFAULT})
option(PP_BROWSER_LIBP2P_EXAMPLES "Build in-tree libp2p examples" OFF)
option(PP_BROWSER_LIBP2P_COVERAGE "Enable libp2p gcovr coverage targets" OFF)

if(PP_BROWSER_IS_MOBILE)
  set(PP_BROWSER_BUILD_TESTS_DEFAULT OFF)
else()
  set(PP_BROWSER_BUILD_TESTS_DEFAULT ON)
endif()
option(PP_BROWSER_BUILD_TESTS "Build host unit and integration tests" ${PP_BROWSER_BUILD_TESTS_DEFAULT})

include(dependencies)
include(Progress)
find_package(Threads REQUIRED)
pp_configure_status("Configuring in-tree RmlUi (src/render)...")

# Hard-forked RmlUi (src/render/)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
if(PP_BROWSER_BUILD_TESTS)
  set(RMLUI_TESTS ON CACHE BOOL "Build in-tree RmlUi unit tests" FORCE)
  set(RMLUI_BACKEND "SDL_GL3" CACHE STRING "" FORCE)
  set(RMLUI_SDL_VERSION_MAJOR "3" CACHE STRING "" FORCE)
else()
  set(RMLUI_TESTS OFF CACHE BOOL "Build in-tree RmlUi unit tests" FORCE)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(src/render)
else()
  add_subdirectory(src/render EXCLUDE_FROM_ALL)
endif()
pp_configure_status("RmlUi configured; setting up pp-browser targets...")

pp_configure_status("Configuring in-tree libp2p (src/libp2p)...")
set(PACKAGE_MANAGER vendored CACHE STRING "" FORCE)
if(PP_BROWSER_IS_MOBILE)
  set(TESTING OFF CACHE BOOL "" FORCE)
  set(EXAMPLES OFF CACHE BOOL "" FORCE)
  set(COVERAGE OFF CACHE BOOL "" FORCE)
else()
  set(TESTING ${PP_BROWSER_LIBP2P_TESTING} CACHE BOOL "Build libp2p tests" FORCE)
  set(EXAMPLES ${PP_BROWSER_LIBP2P_EXAMPLES} CACHE BOOL "Build libp2p examples" FORCE)
  set(COVERAGE ${PP_BROWSER_LIBP2P_COVERAGE} CACHE BOOL "libp2p coverage" FORCE)
endif()
set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)
if(PP_BROWSER_LIBP2P_TESTING OR PP_BROWSER_LIBP2P_COVERAGE)
  add_subdirectory(src/libp2p)
else()
  add_subdirectory(src/libp2p EXCLUDE_FROM_ALL)
endif()
pp_configure_status("libp2p configured")

set(PP_BROWSER_BACKEND_SOURCES
  src/render/integration/platform/RmlUi_Platform_SDL.cpp
  src/render/integration/renderer/RmlUi_Renderer_GL3.cpp
  src/render/integration/renderer/TextLoupeRenderer.cpp
  src/render/integration/host/BrowserHost.cpp
  src/render/integration/host/TouchSimOverlay.cpp
)

add_library(pp_rmlui_backend STATIC ${PP_BROWSER_BACKEND_SOURCES})
target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
if(RMLUI_BACKEND_SIMULATE_TOUCH)
  target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_BACKEND_SIMULATE_TOUCH)
endif()
target_include_directories(pp_rmlui_backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/fork/Include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/integration/platform
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/integration/renderer
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/integration/host
)
target_link_libraries(pp_rmlui_backend PUBLIC
  RmlUi::Core
  ${PP_BROWSER_SDL3_TARGET}
  ${PP_BROWSER_SDL3_IMAGE_TARGET}
)
include(LinkRmlUi)
pp_browser_link_rmlui_svg_deps(pp_rmlui_backend)
if(UNIX AND NOT APPLE)
  target_link_libraries(pp_rmlui_backend PUBLIC ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
  target_link_libraries(pp_rmlui_backend PUBLIC log GLESv3 EGL)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_configure_status("Configuring tests...")
  include(PpBrowserTesting)
  enable_testing()
  pp_browser_enable_googletest()
endif()

add_subdirectory(src)
