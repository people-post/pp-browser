# Fetch people-post/pp-cpp-common (namespace pp) as target pp_common.
#
# Pin a release tag from that repo's main line (PP_CPP_COMMON_GIT_TAG).
# Do not track develop/main branch tips.
#
# Optional override: PP_CPP_COMMON_SOURCE_DIR (local checkout path).

if(TARGET pp_common)
  return()
endif()

include(FetchContent)

set(PP_CPP_COMMON_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-common (overrides FetchContent)")
set(PP_CPP_COMMON_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-common.git"
  CACHE STRING "Git remote for pp-cpp-common")
set(PP_CPP_COMMON_GIT_TAG "v0.2.3"
  CACHE STRING "Release tag on pp-cpp-common main (not a branch name)")

# Shared lib tests are owned by that repo; do not build them inside pp-browser.
set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests" FORCE)

if(PP_CPP_COMMON_SOURCE_DIR AND EXISTS "${PP_CPP_COMMON_SOURCE_DIR}/CMakeLists.txt")
  add_subdirectory("${PP_CPP_COMMON_SOURCE_DIR}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_cpp_common-build" EXCLUDE_FROM_ALL)
else()
  if(PP_CPP_COMMON_SOURCE_DIR)
    message(WARNING "PP_CPP_COMMON_SOURCE_DIR=${PP_CPP_COMMON_SOURCE_DIR} is missing; falling back to FetchContent")
  endif()
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
