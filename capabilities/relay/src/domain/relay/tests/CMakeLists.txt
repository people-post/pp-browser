include(PpBrowserTesting)

add_executable(pp_browser_domain_relay_test
  relay_service_test.cpp
  relay_settings_test.cpp
  relay_store_test.cpp)
target_link_libraries(pp_browser_domain_relay_test PRIVATE
  pp_domain_relay
  SQLite::SQLite3
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_relay_test)
pp_browser_add_gtest(pp_browser_domain_relay_test)
