# Add people-post/pp-cpp-ui (first-party ui:: engine + FreeType/HarfBuzz/LunaSVG + SDL/GL), at its pin
# (cmake/PpPins.cmake; PP_CPP_UI_GIT_TAG, a release tag on that repo's main line), a local checkout
# (PP_CPP_UI_SOURCE_DIR) or a sibling (PP_SIBLINGS_DIR) — to integrate unreleased main before cutting a pp-cpp-ui
# release. A ../pp-cpp-ui beside the repo is no longer picked up by itself (CR012: one switch for every library).
#
# Include after cmake/dependencies.cmake so FreeType can reuse libp2p zlib;
# this file exports PP_BROWSER_SDL3_* for platform/media/render.
# Targets: pp_ui / pp_ui_core / pp_ui_backend / ui::core (paths PP_LIB_UI_*).

include(PpBrowserCapability) # PP_BROWSER_ROOT

# UI engine unit tests run in pp-cpp-ui CI (PP_UI_BUILD_TESTS), not in this repo.
# Enabling UI_TESTS here registers ctest entries under EXCLUDE_FROM_ALL and
# leaves ui_unit_tests / *_NOT_BUILT stubs that fail browser CI.
set(PP_UI_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-ui standalone tests" FORCE)
set(UI_TESTS OFF CACHE BOOL "Build UI engine unit tests" FORCE)

# FreeType: no WOFF2 (brotli). pp-cpp-ui v0.2.1+ always vendors zlib+libpng.
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)

# The implicit sibling this file used to take (until CR012): say why it is not built any more, once per build tree.
if(NOT PP_CPP_UI_SOURCE_DIR AND NOT PP_SIBLINGS_DIR AND NOT PP_BROWSER_PACKAGED_BUILD
   AND EXISTS "${PP_BROWSER_ROOT}/../pp-cpp-ui/CMakeLists.txt" AND NOT PP_CPP_UI_SIBLING_NOTICE_SHOWN)
  message(NOTICE "pp-browser: ../pp-cpp-ui is beside the repo but not built: the pinned ${PP_CPP_UI_GIT_TAG} is. "
                 "To build the checkout: -DPP_SIBLINGS_DIR=.. (every library) or -DPP_CPP_UI_SOURCE_DIR=<path>")
  set(PP_CPP_UI_SIBLING_NOTICE_SHOWN ON CACHE INTERNAL "")
endif()

pp_pin_make_available(pp-cpp-ui)

if(NOT TARGET pp_ui OR NOT TARGET pp_ui_core OR NOT TARGET pp_ui_backend OR NOT TARGET ui::core)
  message(FATAL_ERROR "pp-cpp-ui did not define pp_ui / pp_ui_core / pp_ui_backend / ui::core")
endif()

if(NOT PP_UI_SDL3_TARGET OR NOT PP_UI_SDL3_IMAGE_TARGET)
  message(FATAL_ERROR "pp-cpp-ui did not export PP_UI_SDL3_TARGET / PP_UI_SDL3_IMAGE_TARGET")
endif()

set(PP_BROWSER_SDL3_TARGET "${PP_UI_SDL3_TARGET}" CACHE STRING "SDL3 link target" FORCE)
set(PP_BROWSER_SDL3_IMAGE_TARGET "${PP_UI_SDL3_IMAGE_TARGET}" CACHE STRING "SDL3_image link target" FORCE)

if(NOT TARGET SDL::SDL)
  add_library(SDL_alias INTERFACE)
  add_library(SDL::SDL ALIAS SDL_alias)
  target_link_libraries(SDL_alias INTERFACE ${PP_BROWSER_SDL3_TARGET})
  target_compile_definitions(SDL_alias INTERFACE UI_SDL_VERSION_MAJOR=3)
endif()

if(NOT TARGET SDL_image::SDL_image)
  add_library(SDL_image_alias INTERFACE)
  add_library(SDL_image::SDL_image ALIAS SDL_image_alias)
  target_link_libraries(SDL_image_alias INTERFACE ${PP_BROWSER_SDL3_IMAGE_TARGET})
endif()

# Keep helper names used by existing CMakeLists.
function(pp_browser_link_rmlui_core target)
  target_link_libraries(${target} PRIVATE ui::core)
  if(COMMAND pp_ui_link_svg_deps)
    pp_ui_link_svg_deps(${target})
  elseif(WIN32 AND TARGET lunasvg::lunasvg)
    target_link_libraries(${target} PRIVATE lunasvg::lunasvg)
  endif()
endfunction()

function(pp_browser_link_rmlui_svg_deps target)
  if(COMMAND pp_ui_link_svg_deps)
    pp_ui_link_svg_deps(${target})
  elseif(WIN32 AND TARGET lunasvg::lunasvg)
    target_link_libraries(${target} PRIVATE lunasvg::lunasvg)
  endif()
endfunction()

# Android: SDL's Java glue (android/app/src/main/java/org/libsdl/app/, a copy of SDL's
# android-project sources with pp-browser's few edits) must be the native SDL's version. SDL
# registers its JNI methods when the library loads, before its own runtime version check, so a
# mismatch aborts every launch (SDL 3.4's nativeSetupJNI()V against 3.2's ()I) — fail here instead.
if(ANDROID)
  set(_pp_sdl_version_h "${PP_LIB_UI_ROOT}/third_party/sdl3/include/SDL3/SDL_version.h")
  set(_pp_sdl_activity "${PP_BROWSER_ROOT}/android/app/src/main/java/org/libsdl/app/SDLActivity.java")
  if(NOT EXISTS "${_pp_sdl_version_h}" OR NOT EXISTS "${_pp_sdl_activity}")
    message(FATAL_ERROR "pp-browser: cannot compare SDL's Java glue with the native SDL "
                        "(${_pp_sdl_version_h}, ${_pp_sdl_activity})")
  endif()
  file(STRINGS "${_pp_sdl_version_h}" _pp_sdl_native REGEX "^#define SDL_(MAJOR|MINOR|MICRO)_VERSION +[0-9]+")
  string(REGEX REPLACE ".*SDL_MAJOR_VERSION +([0-9]+).*SDL_MINOR_VERSION +([0-9]+).*SDL_MICRO_VERSION +([0-9]+).*"
         "\\1.\\2.\\3" _pp_sdl_native "${_pp_sdl_native}")
  file(STRINGS "${_pp_sdl_activity}" _pp_sdl_java REGEX "SDL_(MAJOR|MINOR|MICRO)_VERSION = [0-9]+;")
  string(REGEX REPLACE ".*SDL_MAJOR_VERSION = ([0-9]+).*SDL_MINOR_VERSION = ([0-9]+).*SDL_MICRO_VERSION = ([0-9]+).*"
         "\\1.\\2.\\3" _pp_sdl_java "${_pp_sdl_java}")
  if(NOT _pp_sdl_java STREQUAL _pp_sdl_native)
    message(FATAL_ERROR
      "pp-browser: SDL's Java glue is ${_pp_sdl_java} but pp-cpp-ui's SDL is ${_pp_sdl_native}: the app "
      "would abort at launch. Copy pp-cpp-ui's third_party/sdl3/android-project/app/src/main/java/"
      "org/libsdl/app/ into android/app/src/main/java/org/libsdl/app/ and re-apply the edits marked "
      "\"pp-browser:\" (SDLSurface: the keyboard in the safe-area insets).")
  endif()
  message(STATUS "pp-browser: SDL Java glue matches the native SDL (${_pp_sdl_native})")
  unset(_pp_sdl_version_h)
  unset(_pp_sdl_activity)
  unset(_pp_sdl_native)
  unset(_pp_sdl_java)
endif()
