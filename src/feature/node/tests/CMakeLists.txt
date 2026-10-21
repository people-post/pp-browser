include(PpBrowserTesting)

add_executable(pp_browser_feature_node_test node_mesh_services_test.cpp)
target_link_libraries(pp_browser_feature_node_test PRIVATE
  pp_feature_node
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_feature_node_test)
pp_browser_add_gtest(pp_browser_feature_node_test)

if(TARGET pp_feature_node_ledger)
  add_executable(pp_browser_feature_node_ledger_test node_ledger_role_test.cpp)
  target_link_libraries(pp_browser_feature_node_ledger_test PRIVATE
    pp_feature_node_ledger
    pp_mesh_testing
    pp_server  # AmpLedgerServer::protocolIds
    GTest::gtest
    GTest::gtest_main)
  pp_browser_apply_warnings(pp_browser_feature_node_ledger_test)
  pp_browser_add_gtest(pp_browser_feature_node_ledger_test)
endif()
