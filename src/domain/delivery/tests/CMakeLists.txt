include(PpBrowserTesting)

# The transports over real Amp stacks, through the mesh's test harness.
file(GLOB _delivery_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(pp_browser_domain_delivery_test ${_delivery_tests})
target_link_libraries(pp_browser_domain_delivery_test PRIVATE
  pp_domain_delivery
  pp_domain_node
  pp_mesh_testing
  pp_foundation_identity
  pp_lib_net_testing
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_delivery_test)
pp_browser_add_gtest(pp_browser_domain_delivery_test)
