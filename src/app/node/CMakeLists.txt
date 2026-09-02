if(PP_BROWSER_IS_MOBILE)
  return()
endif()

include(PpBrowserWarnings)

add_executable(pp-node
  main.cpp
  NodeBootstrap.cpp
  NodeEnvOverlay.cpp
  NodeMeshPublish.cpp
  StatusHttpProtocol.cpp
  StatusHttpServer.cpp
  # Registration HTTP orchestration (N027); HTTP/sign helpers via pp_domain_net.
  ${CMAKE_SOURCE_DIR}/src/feature/messaging/RegistrationClientUtil.cpp
)

target_include_directories(pp-node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pp-node PRIVATE
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

install(TARGETS pp-node RUNTIME DESTINATION bin)

# Amp thin-client probes (A017) — ephemeral AmpStack, no Libp2pHost.
set(_pp_probe_libs
  pp_domain_mesh
  pp_base_mesh_link
  pp_foundation_crypto
  pp_base_adp
  pp_pbr_common
  Threads::Threads
  sodium
)
if(TARGET OpenSSL::Crypto)
  list(APPEND _pp_probe_libs OpenSSL::Crypto)
endif()

add_executable(pp-node-probe probe/main.cpp)
target_include_directories(pp-node-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp-node-probe PRIVATE ${_pp_probe_libs})
pp_browser_apply_warnings(pp-node-probe)
install(TARGETS pp-node-probe RUNTIME DESTINATION bin)

# call_probe needs AmpDirectChatService (--with-chat); skip in headless (no messaging).
if(TARGET pp_feature_messaging OR TARGET pp_domain_messaging)
  add_executable(pp-call-probe call_probe/main.cpp)
  target_include_directories(pp-call-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(pp-call-probe PRIVATE ${_pp_probe_libs})
  if(TARGET pp_feature_messaging)
    target_link_libraries(pp-call-probe PRIVATE pp_feature_messaging)
  else()
    target_sources(pp-call-probe PRIVATE
      ${CMAKE_SOURCE_DIR}/src/feature/messaging/AmpDirectChatService.cpp)
    target_link_libraries(pp-call-probe PRIVATE pp_domain_messaging)
  endif()
  pp_browser_apply_warnings(pp-call-probe)
  install(TARGETS pp-call-probe RUNTIME DESTINATION bin)
endif()

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
