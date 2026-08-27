# Fetch / add people-post/pp-cpp-ui (RmlUi fork + FreeType / HarfBuzz / LunaSVG).
#
# Prefer a local sibling checkout when present (ppweb3 workspace layout).
# Otherwise pin a release tag from that repo's main line (PP_CPP_UI_GIT_TAG).

include(FetchContent)

set(PP_CPP_UI_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-ui (overrides FetchContent)")
set(PP_CPP_UI_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-ui.git"
  CACHE STRING "Git remote for pp-cpp-ui")
set(PP_CPP_UI_GIT_TAG "v0.1.0"
  CACHE STRING "Release tag on pp-cpp-ui main (not a branch name)")

set(PP_UI_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-ui standalone tests" FORCE)

# Match former in-tree profile: build RmlUi tests when the browser builds tests.
if(PP_BROWSER_BUILD_TESTS)
  set(RMLUI_TESTS ON CACHE BOOL "Build RmlUi unit tests" FORCE)
  set(RMLUI_BACKEND "SDL_GL3" CACHE STRING "" FORCE)
  set(RMLUI_SDL_VERSION_MAJOR "3" CACHE STRING "" FORCE)
endif()

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

if(NOT TARGET pp_ui OR NOT TARGET RmlUi::Core)
  message(FATAL_ERROR "pp-cpp-ui did not define pp_ui / RmlUi::Core")
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
