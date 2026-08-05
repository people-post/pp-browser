if(PP_BROWSER_IS_MOBILE)
  return()
endif()

add_executable(pp-node
  main.cpp
  NodeBootstrap.cpp
  StatusHttpProtocol.cpp
  StatusHttpServer.cpp
)

target_include_directories(pp-node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/libp2p/fork/include
)

target_link_libraries(pp-node PRIVATE
  pp_libp2p_integration
  pp_base_people
  pp_base_crypto
  pp_base_data
  pp_base_platform_core
  pp_base_runtime_core
  pp_base_error
  pp_common
  nlohmann_json::nlohmann_json
  Threads::Threads
  sodium
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-node PRIVATE OpenSSL::Crypto)
endif()

install(TARGETS pp-node RUNTIME DESTINATION bin)

if(PP_BROWSER_BUILD_TESTS)
  add_executable(pp_browser_pp_node_test
    tests/status_http_protocol_test.cpp
    StatusHttpProtocol.cpp
  )
  target_include_directories(pp_browser_pp_node_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(pp_browser_pp_node_test PRIVATE
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    pp_common
  )
  include(PpBrowserTesting)
  pp_browser_add_gtest(pp_browser_pp_node_test)
endif()
