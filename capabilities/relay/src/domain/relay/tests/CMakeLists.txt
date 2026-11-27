include(PpBrowserTesting)

add_executable(pp_browser_domain_relay_test relay_services_test.cpp)
target_link_libraries(pp_browser_domain_relay_test PRIVATE
  pp_domain_relay
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_relay_test)
pp_browser_add_gtest(pp_browser_domain_relay_test)
