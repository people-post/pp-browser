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
pp_browser_apply_warnings(pp_app_node)

# The two node binaries, each with its set of services (cmake/PpNodeServices.cmake; projects/node-relay NR001):
# pp-node, what everyone who helps the mesh runs (the ledger role), and pp-node-plus, with the add-ons (the relay).
# -DPP_NODE_SERVICES=… / -DPP_NODE_PLUS_SERVICES=… build either with another set. A desktop tree builds pp-node-plus only
# when asked (--target pp-node-plus); the headless build (the package script, CI's node-editions job) builds both.
include(PpNodeServices)
pp_add_node_binary(pp-node "${PP_NODE_SERVICES}")
if(PP_BROWSER_HEADLESS)
  pp_add_node_binary(pp-node-plus "${PP_NODE_PLUS_SERVICES}")
else()
  pp_add_node_binary(pp-node-plus "${PP_NODE_PLUS_SERVICES}" EXCLUDE_FROM_ALL)
endif()

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
