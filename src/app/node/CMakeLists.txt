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

# The add-on capabilities pp-node-plus links (projects/node-relay NR001): one library each.
set(_pp_add_ons pp_domain_relay)
# Every add-on's code is in namespace pbr::plus: the mangled prefix tells it apart in a symbol listing.
set(_pp_add_on_symbols "3pbr4plus")
set(_pp_add_on_why "The default pp-node never links the add-on services (projects/node-relay NR001).")

# The default node: what everyone who helps the mesh runs (the ledger role included). It links no add-on.
add_executable(pp-node main.cpp)
target_link_libraries(pp-node PRIVATE pp_app_node)
pp_browser_apply_warnings(pp-node)
# Fail configure / link if a PUBLIC_LIBS edge reintroduces SDL/RmlUi/GUI.
pp_browser_assert_no_ui_dependencies(pp-node)
# First-party closure stays foundation / common / lib {people,net} / headless domain.
pp_browser_assert_node_link_allowlist(pp-node)
pp_browser_add_no_ui_link_check(pp-node)
pp_browser_add_symbol_check(pp-node FORBID ${_pp_add_on_symbols} WHY "${_pp_add_on_why}")

# pp-node-plus: the same node with the add-on services, the same commit and version. A desktop tree builds it only
# when asked (--target pp-node-plus); the headless build (the package script, CI's node-editions job) builds both.
if(PP_BROWSER_HEADLESS)
  add_executable(pp-node-plus main.cpp)
else()
  add_executable(pp-node-plus EXCLUDE_FROM_ALL main.cpp)
endif()
target_compile_definitions(pp-node-plus PRIVATE PP_NODE_PLUS)
target_link_libraries(pp-node-plus PRIVATE pp_app_node ${_pp_add_ons})
pp_browser_apply_warnings(pp-node-plus)
pp_browser_assert_no_ui_dependencies(pp-node-plus)
set(_pp_add_on_patterns "")
foreach(_pp_add_on IN LISTS _pp_add_ons)
  list(APPEND _pp_add_on_patterns "^${_pp_add_on}$")
endforeach()
pp_browser_assert_node_link_allowlist(pp-node-plus EXTRA ${_pp_add_on_patterns})
pp_browser_add_no_ui_link_check(pp-node-plus)
pp_browser_add_symbol_check(pp-node-plus REQUIRE ${_pp_add_on_symbols} WHY "${_pp_add_on_why}")

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
