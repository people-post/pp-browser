if(PP_BROWSER_IS_MOBILE)
  return()
endif()

include(PpBrowserWarnings)
include(PpBrowserNoUiDeps)

add_executable(pp-node
  main.cpp
  NodeBootstrap.cpp
  NodeEnvOverlay.cpp
  NodeMeshPublish.cpp
  NodeMetrics.cpp
  StatusHttpProtocol.cpp
  StatusHttpServer.cpp
)

target_include_directories(pp-node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pp-node PRIVATE
  pp_feature_registration
  pp_feature_node
  pp_domain_mesh
  pp_domain_people
  pp_domain_net
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
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-node PRIVATE OpenSSL::Crypto)
endif()
pp_browser_apply_warnings(pp-node)
# Fail configure / link if a PUBLIC_LIBS edge reintroduces SDL/RmlUi/GUI.
pp_browser_assert_no_ui_dependencies(pp-node)
# First-party closure stays foundation / common / domain {mesh,people,net} / headless feature.
pp_browser_assert_node_link_allowlist(pp-node)
pp_browser_add_no_ui_link_check(pp-node)

install(TARGETS pp-node RUNTIME DESTINATION bin)

# Lab / smoke tools (not shipped in the node image): probes and the product-stack call probe.
add_subdirectory(tools)

if(PP_BROWSER_BUILD_TESTS)
  add_executable(pp_browser_pp_node_test
    tests/status_http_protocol_test.cpp
    tests/node_env_overlay_test.cpp
    StatusHttpProtocol.cpp
    NodeEnvOverlay.cpp
  )
  target_include_directories(pp_browser_pp_node_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(pp_browser_pp_node_test PRIVATE
    GTest::gtest
    GTest::gtest_main
    pp_pbr_common
  )
  pp_browser_apply_warnings(pp_browser_pp_node_test)
  include(PpBrowserTesting)
  pp_browser_add_gtest(pp_browser_pp_node_test)
endif()
