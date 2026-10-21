# Fetch people-post/pp-ledger for pp-node's ledger role (pp-ledger projects/platform-integration pi4):
# one relay or miner per node, on the node's mesh runtime. Target: pp_server (pp::server, with
# RoleRunner); only pp_feature_node_ledger links it, privately.
#
# Pin a release tag from pp-ledger's main line (PP_LEDGER_GIT_TAG, its `release/vX.Y.Z` names);
# override with PP_LEDGER_SOURCE_DIR (local checkout). Include after pp-cpp-common, pp-cpp-crypto and
# pp-cpp-amp: pp-ledger takes those targets from here. Off on mobile (no pp-node there).

if(PP_BROWSER_IS_MOBILE)
  set(_pp_ledger_default OFF)
else()
  set(_pp_ledger_default ON)
endif()
option(PP_BROWSER_WITH_LEDGER "pp-node can run a pp-ledger relay or miner (--ledger-role)" ${_pp_ledger_default})
if(NOT PP_BROWSER_WITH_LEDGER OR TARGET pp_server)
  return()
endif()

include(FetchContent)
include(PpFetchPin)

set(PP_LEDGER_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-ledger (overrides FetchContent)")
set(PP_LEDGER_GIT_REPOSITORY "https://github.com/people-post/pp-ledger.git"
  CACHE STRING "Git remote for pp-ledger")
pp_fetch_git_tag(PP_LEDGER_GIT_TAG "release/v1.7.0"
  "Release tag on pp-ledger main (not a branch name)")

# pp-ledger's own apps, HTTP server and tests build in its repo.
set(PP_LEDGER_BUILD_APPS OFF CACHE BOOL "Build pp-ledger's apps" FORCE)
set(PP_LEDGER_BUILD_HTTP OFF CACHE BOOL "Build pp-ledger's HTTP API server" FORCE)
set(PP_LEDGER_BUILD_TESTS OFF CACHE BOOL "Build pp-ledger's tests" FORCE)

if(PP_LEDGER_SOURCE_DIR AND EXISTS "${PP_LEDGER_SOURCE_DIR}/CMakeLists.txt")
  add_subdirectory("${PP_LEDGER_SOURCE_DIR}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_ledger-build" EXCLUDE_FROM_ALL)
else()
  if(PP_LEDGER_SOURCE_DIR)
    message(WARNING "PP_LEDGER_SOURCE_DIR=${PP_LEDGER_SOURCE_DIR} is missing; falling back to FetchContent")
  endif()
  FetchContent_Declare(
    pp_ledger
    GIT_REPOSITORY ${PP_LEDGER_GIT_REPOSITORY}
    GIT_TAG ${PP_LEDGER_GIT_TAG}
  )
  FetchContent_MakeAvailable(pp_ledger)
endif()

if(NOT TARGET pp_server)
  message(FATAL_ERROR "pp-ledger did not define target pp_server")
endif()
