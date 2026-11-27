if(PP_BROWSER_IS_MOBILE)
  return()
endif()

include(PpBrowserWarnings)
include(PpBrowserNoUiDeps)

# The node app's sources but main: both editions link them (projects/node-relay NR001).
add_library(pp_app_node STATIC
  NodeBootstrap.cpp
  NodeCompatFloor.cpp
  NodeEnvOverlay.cpp
  NodeHeartbeat.cpp
  NodeMeshPublish.cpp
  NodeMetrics.cpp
  NodeUpgradeNotice.cpp
  StatusHttpProtocol.cpp
  StatusHttpServer.cpp
)
target_include_directories(pp_app_node PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(pp_app_node PUBLIC
  pp_domain_registration
  pp_domain_node
  pp_mesh
  pp_lib_people
  pp_lib_net
  pp_foundation_crypto
  pp_foundation_data
  pp_foundation_platform_core
  pp_foundation_runtime_core
  pp_foundation_error
  pp_pbr_common
  Asio::asio
  Threads::Threads
  sodium
  CURL::libcurl
)
if(TARGET pp_domain_node_ledger)
  target_link_libraries(pp_app_node PUBLIC pp_domain_node_ledger)
endif()
pp_browser_apply_warnings(pp_app_node)

# The node services by tier (projects/node-relay NR001): a service is a capability with library pp_domain_<name>, code
# in namespace pbr::<name> and PP_NODE_HAS_<NAME> on its library. Moving one between pp-node and pp-node-plus is moving
# its name between these two lists: nothing on the wire, on disk or in the config names the tier. (The ledger role is
# core and wired on its own: pp_domain_node_ledger, above.)
set(_pp_core_services)
set(_pp_plus_services relay)

# Each service's library, its allowlist entry, and what tells its code apart in a symbol listing (the mangled
# namespace, e.g. pbr::relay → 3pbr5relay).
function(_pp_node_services out_libs out_allow out_symbols)
  set(_libs "")
  set(_allow "")
  set(_symbols "")
  foreach(_service IN LISTS ARGN)
    string(LENGTH "${_service}" _len)
    list(APPEND _libs pp_domain_${_service})
    list(APPEND _allow "^pp_domain_${_service}$")
    list(APPEND _symbols "3pbr${_len}${_service}")
  endforeach()
  set(${out_libs} "${_libs}" PARENT_SCOPE)
  set(${out_allow} "${_allow}" PARENT_SCOPE)
  set(${out_symbols} "${_symbols}" PARENT_SCOPE)
endfunction()
_pp_node_services(_pp_core_libs _pp_core_allow _pp_core_symbols ${_pp_core_services})
_pp_node_services(_pp_plus_libs _pp_plus_allow _pp_plus_symbols ${_pp_plus_services})
set(_pp_plus_why "The default pp-node never links pp-node-plus's add-on services (projects/node-relay NR001).")

# The default node: what everyone who helps the mesh runs (the ledger role included). It links no add-on.
add_executable(pp-node main.cpp)
target_link_libraries(pp-node PRIVATE pp_app_node ${_pp_core_libs})
pp_browser_apply_warnings(pp-node)
# Fail configure / link if a PUBLIC_LIBS edge reintroduces SDL/RmlUi/GUI.
pp_browser_assert_no_ui_dependencies(pp-node)
# First-party closure stays foundation / common / lib {people,net} / headless domain (and the core services).
pp_browser_assert_node_link_allowlist(pp-node EXTRA ${_pp_core_allow})
pp_browser_add_no_ui_link_check(pp-node)
pp_browser_add_symbol_check(pp-node FORBID ${_pp_plus_symbols} REQUIRE ${_pp_core_symbols} WHY "${_pp_plus_why}")

# pp-node-plus: the same node with the add-on services, the same commit and version. A desktop tree builds it only
# when asked (--target pp-node-plus); the headless build (the package script, CI's node-editions job) builds both.
if(PP_BROWSER_HEADLESS)
  add_executable(pp-node-plus main.cpp)
else()
  add_executable(pp-node-plus EXCLUDE_FROM_ALL main.cpp)
endif()
target_compile_definitions(pp-node-plus PRIVATE PP_NODE_PLUS)
target_link_libraries(pp-node-plus PRIVATE pp_app_node ${_pp_core_libs} ${_pp_plus_libs})
pp_browser_apply_warnings(pp-node-plus)
pp_browser_assert_no_ui_dependencies(pp-node-plus)
pp_browser_assert_node_link_allowlist(pp-node-plus EXTRA ${_pp_core_allow} ${_pp_plus_allow})
pp_browser_add_no_ui_link_check(pp-node-plus)
pp_browser_add_symbol_check(pp-node-plus REQUIRE ${_pp_core_symbols} ${_pp_plus_symbols} WHY "${_pp_plus_why}")

install(TARGETS pp-node RUNTIME DESTINATION bin)

# Lab / smoke tools (not shipped in the node image): probes and the product-stack call probe.
add_subdirectory(tools)

if(PP_BROWSER_BUILD_TESTS)
  add_executable(pp_browser_pp_node_test
    tests/status_http_protocol_test.cpp
    tests/node_env_overlay_test.cpp
    tests/node_heartbeat_test.cpp
    tests/status_http_server_test.cpp
    tests/node_upgrade_notice_test.cpp
    tests/node_compat_floor_test.cpp
    NodeCompatFloor.cpp
    NodeHeartbeat.cpp
    NodeUpgradeNotice.cpp
    StatusHttpProtocol.cpp
    StatusHttpServer.cpp
    NodeEnvOverlay.cpp
  )
  target_include_directories(pp_browser_pp_node_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(pp_browser_pp_node_test PRIVATE
    GTest::gtest
    GTest::gtest_main
    pp_pbr_common
    pp_foundation_tls
    pp_lib_net
    Asio::asio
    Threads::Threads
  )
  pp_browser_apply_warnings(pp_browser_pp_node_test)
  include(PpBrowserTesting)
  pp_browser_add_gtest(pp_browser_pp_node_test)
endif()
