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
  # Prevent windows.h min/max macros from breaking std::min/std::max (soralog, etc.).
  add_compile_definitions(NOMINMAX)
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

add_subdirectory(src/render EXCLUDE_FROM_ALL)
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
  src/render/integration/host/BrowserHost.cpp
)

add_library(pp_rmlui_backend STATIC ${PP_BROWSER_BACKEND_SOURCES})
target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
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

add_subdirectory(src/common)
add_subdirectory(src/base)

set(PP_BROWSER_APP_SOURCES
  src/app/main.cpp
  src/app/Application.cpp
  src/app/Bootstrap.cpp
)

add_subdirectory(src/feature)

if(ANDROID)
  add_library(${PP_BROWSER_APP_TARGET} SHARED ${PP_BROWSER_APP_SOURCES})
  set_target_properties(${PP_BROWSER_APP_TARGET} PROPERTIES OUTPUT_NAME main)
else()
  add_executable(${PP_BROWSER_APP_TARGET} ${PP_BROWSER_APP_SOURCES})
endif()

option(PP_BROWSER_PACKAGED_BUILD "Build for installable/packaged release (runtime asset paths)" OFF)

set(_pp_browser_compile_defs RMLUI_STATIC_LIB)
if(PP_BROWSER_PACKAGED_BUILD OR PP_BROWSER_IS_MOBILE)
  list(APPEND _pp_browser_compile_defs PP_BROWSER_PACKAGED_BUILD=1)
elseif(NOT PP_BROWSER_IS_MOBILE)
  list(APPEND _pp_browser_compile_defs PP_BROWSER_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()

target_compile_definitions(${PP_BROWSER_APP_TARGET} PRIVATE ${_pp_browser_compile_defs})

if(WIN32)
  set_target_properties(${PP_BROWSER_APP_TARGET} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE AND PP_BROWSER_PACKAGED_BUILD AND NOT PP_BROWSER_IS_IOS)
  set_target_properties(${PP_BROWSER_APP_TARGET} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.pp-browser.app"
    MACOSX_BUNDLE_BUNDLE_NAME "pp-browser"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PP_BROWSER_RELEASE_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PP_BROWSER_RELEASE_VERSION}"
  )
endif()

target_include_directories(${PP_BROWSER_APP_TARGET} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libp2p/fork/include
)

target_link_libraries(${PP_BROWSER_APP_TARGET} PRIVATE
  pp_feature
  pp_base
  pp_common
  pp_rmlui_backend
  RmlUi::Core
  p2p
  nlohmann_json::nlohmann_json
  CURL::libcurl
  Threads::Threads
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(${PP_BROWSER_APP_TARGET} PRIVATE OpenSSL::Crypto)
endif()

option(PPBROWSER_ENABLE_DEBUGGER "Link RmlUi debugger" ${PPBROWSER_ENABLE_DEBUGGER_DEFAULT})
option(PPBROWSER_LOCAL_LLM "Enable local LLM backend (not implemented)" OFF)
if(PPBROWSER_LOCAL_LLM)
  message(STATUS "PPBROWSER_LOCAL_LLM requested; implementation deferred")
endif()
if(PPBROWSER_ENABLE_DEBUGGER)
  target_link_libraries(${PP_BROWSER_APP_TARGET} PRIVATE RmlUi::Debugger)
  target_compile_definitions(${PP_BROWSER_APP_TARGET} PRIVATE PPBROWSER_ENABLE_DEBUGGER=1)
endif()

if(NOT PP_BROWSER_IS_MOBILE)
  pp_configure_status("Configuring tests...")
  include(LinkRmlUi)
  enable_testing()
  add_subdirectory(tests)
  pp_configure_status("Configure step complete")

  if(APPLE AND PP_BROWSER_PACKAGED_BUILD)
    install(TARGETS ${PP_BROWSER_APP_TARGET} BUNDLE DESTINATION .)
    install(DIRECTORY assets/ DESTINATION pp-browser.app/Contents/Resources/assets)
  elseif(WIN32 AND PP_BROWSER_PACKAGED_BUILD)
    install(TARGETS ${PP_BROWSER_APP_TARGET} RUNTIME DESTINATION .)
    install(DIRECTORY assets/ DESTINATION assets)
  else()
    install(TARGETS ${PP_BROWSER_APP_TARGET} RUNTIME DESTINATION bin)
    install(DIRECTORY assets/ DESTINATION share/pp-browser/assets)
  endif()

  if(PP_BROWSER_PACKAGED_BUILD AND (APPLE OR WIN32))
    include(CPackOptions)
  endif()
else()
  pp_configure_status("Mobile target: skipping host tests and desktop install rules")
endif()

# Alias for scripts/docs that refer to pp-browser on all platforms.
if(ANDROID AND NOT TARGET pp-browser)
  add_library(pp-browser ALIAS main)
endif()
