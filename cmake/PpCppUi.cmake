# Fetch / add people-post/pp-cpp-ui (RmlUi + FreeType/HarfBuzz/LunaSVG + SDL/GL backend).
#
# Prefer a local sibling checkout when present (ppweb3 workspace layout).
# Otherwise pin a release tag from that repo's main line (PP_CPP_UI_GIT_TAG).
#
# Include after cmake/dependencies.cmake so FreeType can reuse libp2p zlib;
# this file exports PP_BROWSER_SDL3_* for platform/media/render.

include(FetchContent)

set(PP_CPP_UI_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-ui (overrides FetchContent)")
set(PP_CPP_UI_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-ui.git"
  CACHE STRING "Git remote for pp-cpp-ui")
set(PP_CPP_UI_GIT_TAG "v0.2.0"
  CACHE STRING "Release tag on pp-cpp-ui main (not a branch name)")

# RmlUi unit tests run in pp-cpp-ui CI (PP_UI_BUILD_TESTS), not in this repo.
# Enabling RMLUI_TESTS here registers ctest entries under EXCLUDE_FROM_ALL and
# leaves rmlui_unit_tests / *_NOT_BUILT stubs that fail browser CI.
set(PP_UI_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-ui standalone tests" FORCE)
set(RMLUI_TESTS OFF CACHE BOOL "Build RmlUi unit tests" FORCE)

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

if(NOT TARGET pp_ui OR NOT TARGET pp_ui_rml OR NOT TARGET pp_ui_backend OR NOT TARGET RmlUi::Core)
  message(FATAL_ERROR "pp-cpp-ui did not define pp_ui / pp_ui_rml / pp_ui_backend / RmlUi::Core")
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
  target_compile_definitions(SDL_alias INTERFACE RMLUI_SDL_VERSION_MAJOR=3)
endif()

if(NOT TARGET SDL_image::SDL_image)
  add_library(SDL_image_alias INTERFACE)
  add_library(SDL_image::SDL_image ALIAS SDL_image_alias)
  target_link_libraries(SDL_image_alias INTERFACE ${PP_BROWSER_SDL3_IMAGE_TARGET})
endif()

# Keep helper names used by existing CMakeLists.
function(pp_browser_link_rmlui_core target)
  target_link_libraries(${target} PRIVATE RmlUi::Core)
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
