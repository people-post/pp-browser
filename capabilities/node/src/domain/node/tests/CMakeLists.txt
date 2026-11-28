include(PpBrowserTesting)

add_executable(pp_browser_domain_node_test node_mesh_services_test.cpp)
target_link_libraries(pp_browser_domain_node_test PRIVATE
  pp_domain_node
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_node_test)
pp_browser_add_gtest(pp_browser_domain_node_test)
