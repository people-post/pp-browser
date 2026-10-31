include(PpBrowserTesting)

file(GLOB _attachments_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(pp_browser_domain_attachments_test ${_attachments_tests})
target_link_libraries(pp_browser_domain_attachments_test PRIVATE
  pp_domain_attachments
  pp_lib_net_testing
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_attachments_test)
pp_browser_add_gtest(pp_browser_domain_attachments_test)
