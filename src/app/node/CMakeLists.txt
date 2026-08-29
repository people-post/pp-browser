if(PP_BROWSER_IS_MOBILE)
  return()
endif()

include(PpBrowserWarnings)

add_executable(pp-node
  main.cpp
  NodeBootstrap.cpp
  NodeEnvOverlay.cpp
  StatusHttpProtocol.cpp
  StatusHttpServer.cpp
)

target_include_directories(pp-node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(pp-node PRIVATE
  pp_base_p2p
  pp_base_people
  pp_base_crypto
  pp_base_data
  pp_base_platform_core
  pp_base_runtime_core
  pp_base_error
  pp_common
  Threads::Threads
  sodium
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-node PRIVATE OpenSSL::Crypto)
endif()
pp_browser_apply_warnings(pp-node)

install(TARGETS pp-node RUNTIME DESTINATION bin)

add_executable(pp-node-probe
  probe/main.cpp
)
target_include_directories(pp-node-probe PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(pp-node-probe PRIVATE
  pp_base_p2p
  pp_common
  Threads::Threads
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-node-probe PRIVATE OpenSSL::Crypto)
endif()
pp_browser_apply_warnings(pp-node-probe)

add_executable(pp-call-probe
  call_probe/main.cpp
)
target_include_directories(pp-call-probe PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(pp-call-probe PRIVATE
  pp_base_p2p
  pp_common
  Threads::Threads
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-call-probe PRIVATE OpenSSL::Crypto)
endif()
pp_browser_apply_warnings(pp-call-probe)

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
    pp_common
  )
  pp_browser_apply_warnings(pp_browser_pp_node_test)
  include(PpBrowserTesting)
  pp_browser_add_gtest(pp_browser_pp_node_test)
endif()
