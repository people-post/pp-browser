include(PpBrowserTesting)

file(GLOB _peers_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(pp_browser_domain_peers_test ${_peers_tests})
target_link_libraries(pp_browser_domain_peers_test PRIVATE
  pp_domain_peers
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_peers_test)
pp_browser_add_gtest(pp_browser_domain_peers_test)
