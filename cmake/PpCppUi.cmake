# Fetch / add people-post/pp-cpp-ui (first-party ui:: engine + FreeType/HarfBuzz/LunaSVG + SDL/GL).
#
# Prefer a local sibling checkout when present (ppweb3 workspace layout) — use this
# when integrating unreleased develop before cutting a pp-cpp-ui release tag.
# Otherwise pin a release tag from that repo's main line (PP_CPP_UI_GIT_TAG).
#
# Include after cmake/dependencies.cmake so FreeType can reuse libp2p zlib;
# this file exports PP_BROWSER_SDL3_* for platform/media/render.
# Targets: pp_ui / pp_ui_core / pp_ui_backend / ui::core (paths PP_LIB_UI_*).

include(FetchContent)
include(PpFetchPin)

set(PP_CPP_UI_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-ui (overrides FetchContent)")
set(PP_CPP_UI_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-ui.git"
  CACHE STRING "Git remote for pp-cpp-ui")
pp_fetch_git_tag(PP_CPP_UI_GIT_TAG "v0.6.1"
  "Release tag on pp-cpp-ui main (not a branch name)")

# UI engine unit tests run in pp-cpp-ui CI (PP_UI_BUILD_TESTS), not in this repo.
# Enabling UI_TESTS here registers ctest entries under EXCLUDE_FROM_ALL and
# leaves ui_unit_tests / *_NOT_BUILT stubs that fail browser CI.
set(PP_UI_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-ui standalone tests" FORCE)
set(UI_TESTS OFF CACHE BOOL "Build UI engine unit tests" FORCE)

# FreeType: no WOFF2 (brotli). pp-cpp-ui v0.2.1+ always vendors zlib+libpng.
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)

set(_pp_ui_sibling "${CMAKE_SOURCE_DIR}/../pp-cpp-ui")
if(PP_CPP_UI_SOURCE_DIR)
  set(_pp_ui_src "${PP_CPP_UI_SOURCE_DIR}")
elseif(EXISTS "${_pp_ui_sibling}/CMakeLists.txt")
  set(_pp_ui_src "${_pp_ui_sibling}")
  message(STATUS "pp-browser: using sibling pp-cpp-ui at ${_pp_ui_src}")
else()
  set(_pp_ui_src "")
endif()

if(_pp_ui_src)
  add_subdirectory("${_pp_ui_src}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_cpp_ui-build" EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(
    pp_cpp_ui
    GIT_REPOSITORY ${PP_CPP_UI_GIT_REPOSITORY}
    GIT_TAG ${PP_CPP_UI_GIT_TAG}
  )
  FetchContent_MakeAvailable(pp_cpp_ui)
endif()

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

unset(_pp_ui_sibling)
unset(_pp_ui_src)

# Android: SDL's Java glue (android/app/src/main/java/org/libsdl/app/, a copy of SDL's
# android-project sources with pp-browser's few edits) must be the native SDL's version. SDL
# registers its JNI methods when the library loads, before its own runtime version check, so a
# mismatch aborts every launch (SDL 3.4's nativeSetupJNI()V against 3.2's ()I) — fail here instead.
if(ANDROID)
  set(_pp_sdl_version_h "${PP_LIB_UI_ROOT}/third_party/sdl3/include/SDL3/SDL_version.h")
  set(_pp_sdl_activity "${CMAKE_SOURCE_DIR}/android/app/src/main/java/org/libsdl/app/SDLActivity.java")
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
