# Pins: the people-post libraries a build compiles against, each at a release tag (projects/capability-repos
# CR012). The list is the app's (cmake/PpPins.cmake, read once by the root CMakeLists.txt); this file is the
# mechanism, which a repo that keeps its own list (a capabilities repo, pp-ledger-apps) copies beside it.
#
#   pp_pin(pp-cpp-common TAG v1.5.1 [REPOSITORY <url>])
#     records a pin. Its cache variables are named after the repo, upper case, '-' → '_':
#     PP_CPP_COMMON_GIT_TAG (the tag; pp_fetch_git_tag below), PP_CPP_COMMON_GIT_REPOSITORY,
#     PP_CPP_COMMON_SOURCE_DIR (a local checkout, overriding the tag).
#   pp_pin_make_available(pp-cpp-common [FILES_ONLY])
#     adds the library, from the first of: its *_SOURCE_DIR, ${PP_SIBLINGS_DIR}/<repo> (sibling mode), its tag
#     (FetchContent). A local checkout whose HEAD is not the pinned tag is warned about. FILES_ONLY: a repo with
#     nothing to build (pp-specs): its files are fetched or found the same ways, and nothing is added to the build.
#   pp_pin_source_dir(pp-cpp-common <out>)
#     the source tree that build used (for a test that reads a library's own files).
#
# Sibling mode: -DPP_SIBLINGS_DIR=<dir> (relative to the repo root, e.g. `..`) builds every pin from <dir>/<repo>, a
# checkout beside the repo, so a library change and the app build together without a release. A pin with no
# checkout there is fetched at its tag. Never in a packaged build (PB-001: a release compiles its pinned tags).

include(FetchContent)

# Refresh a cached FetchContent GIT_TAG when the pin in CMake source changes,
# without clobbering an intentional -D override (tag != last recorded pin).
#
# Usage:
#   pp_fetch_git_tag(PP_CPP_COMMON_GIT_TAG "v0.2.3"
#     "Release tag on pp-cpp-common main (not a branch name)")
function(pp_fetch_git_tag cache_var pin doc)
  set(_last_pin_var "${cache_var}_LAST_PIN")
  if(NOT DEFINED CACHE{${_last_pin_var}})
    # First configure with pin tracking (or wiped LAST_PIN): adopt the cmake pin.
    # Refreshes stale tags left by older builds that used plain CACHE STRING.
    set(${cache_var} "${pin}" CACHE STRING "${doc}" FORCE)
  elseif("${${cache_var}}" STREQUAL "${${_last_pin_var}}" AND
         NOT "${${cache_var}}" STREQUAL "${pin}")
    # Cache still holds the previous pin; cmake pin moved — follow it.
    set(${cache_var} "${pin}" CACHE STRING "${doc}" FORCE)
  else()
    # Unset → seed; otherwise keep cache (user override or already current).
    set(${cache_var} "${pin}" CACHE STRING "${doc}")
  endif()
  set(${_last_pin_var} "${pin}" CACHE INTERNAL
    "Last ${cache_var} pin from CMake source (auto-refresh)" FORCE)
endfunction()

set(PP_SIBLINGS_DIR "" CACHE PATH
  "Sibling mode: build every pinned library from <dir>/<repo> instead of its tag (relative to the repo root, e.g. ..)")
get_filename_component(PP_PINS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# PP_CPP_COMMON for pp-cpp-common: the prefix of a pin's cache variables.
function(pp_pin_prefix name out)
  string(TOUPPER "${name}" _prefix)
  string(REPLACE "-" "_" _prefix "${_prefix}")
  set(${out} "${_prefix}" PARENT_SCOPE)
endfunction()

function(pp_pin name)
  cmake_parse_arguments(ARG "" "TAG;REPOSITORY" "" ${ARGN})
  if(NOT ARG_TAG)
    message(FATAL_ERROR "pp_pin(${name}): TAG is required")
  endif()
  if(NOT ARG_REPOSITORY)
    set(ARG_REPOSITORY "https://github.com/people-post/${name}.git")
  endif()
  pp_pin_prefix("${name}" _p)
  set(${_p}_SOURCE_DIR "" CACHE PATH "Optional local checkout of ${name} (overrides its pinned tag)")
  set(${_p}_GIT_REPOSITORY "${ARG_REPOSITORY}" CACHE STRING "Git remote for ${name}")
  pp_fetch_git_tag(${_p}_GIT_TAG "${ARG_TAG}" "Release tag on ${name} main (not a branch name)")
  set_property(GLOBAL APPEND PROPERTY PP_PINS "${name}")
endfunction()

# Warn when the local checkout at `dir` is not at `tag`: it builds something else than CI does (a test of newer
# library work fails here and passes there, or the reverse — FontWeightsTest, 2026-11-12).
function(pp_pin_check_head name dir tag)
  find_package(Git QUIET)
  if(NOT GIT_FOUND)
    return()
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
                  OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
  if(NOT _rc EQUAL 0)
    message(WARNING "${PROJECT_NAME}: ${dir} (${name}) is not a git checkout; cannot tell whether it is the pinned ${tag}")
    return()
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse -q --verify "refs/tags/${tag}^{commit}"
                  OUTPUT_VARIABLE _pinned OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
  if(NOT _rc EQUAL 0)
    message(WARNING "${PROJECT_NAME}: the local ${name} has no tag ${tag} (the pin); fetch its tags "
                    "(git -C ${dir} fetch --tags) to tell whether it is the pinned version")
    return()
  endif()
  if(_head STREQUAL _pinned)
    return()
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" describe --tags --always HEAD
                  OUTPUT_VARIABLE _at OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" merge-base --is-ancestor "${_pinned}" HEAD
                  RESULT_VARIABLE _contains OUTPUT_QUIET ERROR_QUIET)
  if(_contains EQUAL 0)
    set(_how "past the pin: this build has library changes CI's does not")
  else()
    set(_how "does not contain the pin: update it (git -C ${dir} checkout ${tag}), or this build differs from CI's")
  endif()
  message(WARNING "${PROJECT_NAME}: the local ${name} is at ${_at}, not the pinned ${tag} (${_how})")
endfunction()

function(pp_pin_make_available name)
  cmake_parse_arguments(ARG "FILES_ONLY" "" "" ${ARGN})
  # How a checkout is recognised: a CMake project, or, for a files-only repo, its README.
  if(ARG_FILES_ONLY)
    set(_marker "README.md")
  else()
    set(_marker "CMakeLists.txt")
  endif()
  get_property(_pins GLOBAL PROPERTY PP_PINS)
  if(NOT name IN_LIST _pins)
    message(FATAL_ERROR "pp_pin_make_available(${name}): no pp_pin(${name} …) in the pin list (cmake/PpPins.cmake)")
  endif()
  pp_pin_prefix("${name}" _p)
  # Once per build, as FetchContent_MakeAvailable: a library's own CMake may include the same cmake/Pp<Lib>.cmake
  # again (pp-ledger includes PpCppCrypto, which resolves to this tree's).
  get_property(_done GLOBAL PROPERTY PP_PIN_SOURCE_DIR_${_p} SET)
  if(_done)
    return()
  endif()
  string(TOLOWER "${_p}" _fc)
  set(_tag "${${_p}_GIT_TAG}")

  set(_src "")
  set(_how "")
  if(${_p}_SOURCE_DIR)
    if(EXISTS "${${_p}_SOURCE_DIR}/${_marker}")
      set(_src "${${_p}_SOURCE_DIR}")
      set(_how "${_p}_SOURCE_DIR")
    else()
      message(WARNING "${_p}_SOURCE_DIR=${${_p}_SOURCE_DIR} is missing; falling back to FetchContent")
    endif()
  elseif(PP_SIBLINGS_DIR)
    if(PP_BROWSER_PACKAGED_BUILD)
      message(FATAL_ERROR "PP_SIBLINGS_DIR is for development: a packaged build compiles the pinned tags (PB-001)")
    endif()
    get_filename_component(_siblings "${PP_SIBLINGS_DIR}" ABSOLUTE BASE_DIR "${PP_PINS_REPO_ROOT}")
    if(EXISTS "${_siblings}/${name}/${_marker}")
      set(_src "${_siblings}/${name}")
      set(_how "sibling")
    else()
      message(WARNING "${PROJECT_NAME}: sibling mode, but no ${_siblings}/${name}; building ${name} at its pin ${_tag}")
    endif()
  endif()

  if(_src)
    message(STATUS "${PROJECT_NAME}: ${name} from ${_src} (${_how}; pin ${_tag})")
    pp_pin_check_head("${name}" "${_src}" "${_tag}")
    if(NOT ARG_FILES_ONLY)
      add_subdirectory("${_src}" "${CMAKE_BINARY_DIR}/_deps/${_fc}-build" EXCLUDE_FROM_ALL)
    endif()
  else()
    set(_git_config "")
    if(ARG_FILES_ONLY)
      # A files-only repo holds data compared byte for byte (pp-specs' vectors): never let git convert line ends,
      # whatever the machine's core.autocrlf (Windows runners default to true and turned LF into CRLF).
      set(_git_config GIT_CONFIG core.autocrlf=false)
    endif()
    FetchContent_Declare(
      ${_fc}
      GIT_REPOSITORY ${${_p}_GIT_REPOSITORY}
      GIT_TAG ${_tag}
      ${_git_config}
    )
    # A repo without a CMakeLists.txt (FILES_ONLY) is only fetched: MakeAvailable adds nothing for it.
    FetchContent_MakeAvailable(${_fc})
    FetchContent_GetProperties(${_fc} SOURCE_DIR _src)
  endif()
  set_property(GLOBAL PROPERTY PP_PIN_SOURCE_DIR_${_p} "${_src}")
endfunction()

function(pp_pin_source_dir name out)
  pp_pin_prefix("${name}" _p)
  get_property(_src GLOBAL PROPERTY PP_PIN_SOURCE_DIR_${_p})
  set(${out} "${_src}" PARENT_SCOPE)
endfunction()
