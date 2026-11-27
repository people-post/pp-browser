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

# What tells the default node from pp-node-relay's relay services in a symbol listing (RelayServices).
set(_pp_relay_symbols "13RelayServices")
set(_pp_relay_why "The default pp-node never links the relay services (projects/node-relay NR001).")

# The default node: what everyone who helps the mesh runs. It does not link the relay services.
add_executable(pp-node main.cpp)
target_link_libraries(pp-node PRIVATE pp_app_node)
pp_browser_apply_warnings(pp-node)
# Fail configure / link if a PUBLIC_LIBS edge reintroduces SDL/RmlUi/GUI.
pp_browser_assert_no_ui_dependencies(pp-node)
# First-party closure stays foundation / common / lib {people,net} / headless domain.
pp_browser_assert_node_link_allowlist(pp-node)
pp_browser_add_no_ui_link_check(pp-node)
pp_browser_add_symbol_check(pp-node FORBID ${_pp_relay_symbols} WHY "${_pp_relay_why}")

# pp-node-relay: the same node with the relay services (the relay capability), the same commit and version. A desktop
# tree builds it only when asked (--target pp-node-relay); the headless build (the package script, CI's node-editions
# job) builds both.
if(PP_BROWSER_HEADLESS)
  add_executable(pp-node-relay main.cpp)
else()
  add_executable(pp-node-relay EXCLUDE_FROM_ALL main.cpp)
endif()
target_compile_definitions(pp-node-relay PRIVATE PP_NODE_RELAY_EDITION)
target_link_libraries(pp-node-relay PRIVATE pp_app_node pp_domain_relay)
pp_browser_apply_warnings(pp-node-relay)
pp_browser_assert_no_ui_dependencies(pp-node-relay)
pp_browser_assert_node_link_allowlist(pp-node-relay EXTRA "^pp_domain_relay$")
pp_browser_add_no_ui_link_check(pp-node-relay)
pp_browser_add_symbol_check(pp-node-relay REQUIRE ${_pp_relay_symbols} WHY "${_pp_relay_why}")

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
