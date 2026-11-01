include(PpBrowserTesting)

# The leg / compose tests drive real Amp stacks through the mesh's test harness.
file(GLOB _call_media_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(pp_browser_domain_call_media_test
  ${_call_media_tests})
target_link_libraries(pp_browser_domain_call_media_test PRIVATE
  pp_domain_call_media
  pp_mesh
  pp_mesh_testing
  pp_lib_people
  pp_foundation_identity
  pp_lib_net
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_domain_call_media_test)
pp_browser_add_gtest(pp_browser_domain_call_media_test)
