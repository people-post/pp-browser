# Fetch / add people-post/pp-cpp-common (namespace pp) as target pp_common.
#
# Prefer a local sibling checkout when present (ppweb3 workspace layout).
# Otherwise pin PP_CPP_COMMON_GIT_TAG (release tag preferred; commit SHA OK until tagged).
# From the json+Service cut, pp_common PUBLIC-links lean nlohmann_json.

include(FetchContent)

set(PP_CPP_COMMON_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-common (overrides FetchContent)")
set(PP_CPP_COMMON_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-common.git"
  CACHE STRING "Git remote for pp-cpp-common")
# Temporary: commit on cursor/json-service-queue-33a3 (json + Service + ThreadSafeQueue).
# Switch to v0.2.0 after that release lands on pp-cpp-common main.
set(PP_CPP_COMMON_GIT_TAG "c3c7cb8270560a037aacf70cbe9094432e2f65ad"
  CACHE STRING "pp-cpp-common git tag or commit (not a floating branch name)")

# Shared lib tests are owned by that repo; do not build them inside pp-browser.
set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests" FORCE)

set(_pp_common_sibling "${CMAKE_SOURCE_DIR}/../pp-cpp-common")
if(PP_CPP_COMMON_SOURCE_DIR AND EXISTS "${PP_CPP_COMMON_SOURCE_DIR}/CMakeLists.txt")
  set(_pp_common_src "${PP_CPP_COMMON_SOURCE_DIR}")
elseif(EXISTS "${_pp_common_sibling}/CMakeLists.txt")
  set(_pp_common_src "${_pp_common_sibling}")
  message(STATUS "pp-browser: using sibling pp-cpp-common at ${_pp_common_src}")
else()
  if(PP_CPP_COMMON_SOURCE_DIR)
    message(WARNING "PP_CPP_COMMON_SOURCE_DIR=${PP_CPP_COMMON_SOURCE_DIR} is missing; falling back to FetchContent")
  endif()
  set(_pp_common_src "")
endif()

if(_pp_common_src)
  add_subdirectory("${_pp_common_src}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_cpp_common-build" EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(
    pp_cpp_common
    GIT_REPOSITORY ${PP_CPP_COMMON_GIT_REPOSITORY}
    GIT_TAG ${PP_CPP_COMMON_GIT_TAG}
  )
  FetchContent_MakeAvailable(pp_cpp_common)
endif()

if(NOT TARGET pp_common)
  message(FATAL_ERROR "pp-cpp-common did not define target pp_common")
endif()

# pp-browser call sites still use namespace pbr; force-include the bridge so
# pbr:: aliases exist whenever common headers are used.
set(PP_BROWSER_PBR_COMPAT_HEADER "${CMAKE_SOURCE_DIR}/src/common/PbrCompat.h")
if(MSVC)
  target_compile_options(pp_common PUBLIC "/FI${PP_BROWSER_PBR_COMPAT_HEADER}")
else()
  target_compile_options(pp_common PUBLIC "-include" "${PP_BROWSER_PBR_COMPAT_HEADER}")
endif()

unset(_pp_common_sibling)
unset(_pp_common_src)
