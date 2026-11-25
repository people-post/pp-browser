# Add people-post/pp-ledger for pp-node's ledger role (pp-ledger projects/platform-integration pi4):
# one relay or miner per node, on the node's mesh runtime. Target: pp_server (pp::server, with
# RoleRunner); only pp_domain_node_ledger links it, privately.
#
# At its pin (cmake/PpPins.cmake; PP_LEDGER_GIT_TAG: a pp-ledger release `vX.Y.Z` or, on main, a candidate
# `vX.Y.Z-rc.N`, RC023; the `release/vX.Y.Z` tags before 2.0.0 were development pins), a local checkout (PP_LEDGER_SOURCE_DIR) or a sibling (PP_SIBLINGS_DIR). Include after pp-cpp-common,
# pp-cpp-crypto and pp-cpp-amp: pp-ledger takes those targets from here. Off on mobile (no pp-node there).

if(PP_BROWSER_IS_MOBILE)
  set(_pp_ledger_default OFF)
else()
  set(_pp_ledger_default ON)
endif()
option(PP_BROWSER_WITH_LEDGER "pp-node can run a pp-ledger relay or miner (--ledger-role)" ${_pp_ledger_default})
if(NOT PP_BROWSER_WITH_LEDGER OR TARGET pp_server)
  return()
endif()

# pp-ledger's own apps, HTTP server and tests build in its repo.
set(PP_LEDGER_BUILD_APPS OFF CACHE BOOL "Build pp-ledger's apps" FORCE)
set(PP_LEDGER_BUILD_HTTP OFF CACHE BOOL "Build pp-ledger's HTTP API server" FORCE)
set(PP_LEDGER_BUILD_TESTS OFF CACHE BOOL "Build pp-ledger's tests" FORCE)

pp_pin_make_available(pp-ledger)

if(NOT TARGET pp_server)
  message(FATAL_ERROR "pp-ledger did not define target pp_server")
endif()
