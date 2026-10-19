include(PpBrowserTesting)

# The leg / compose tests drive real Amp stacks through the mesh's test harness.
file(GLOB _call_media_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(pp_browser_feature_call_media_test
  ${_call_media_tests}
  ${CMAKE_SOURCE_DIR}/src/domain/mesh/tests/support/mesh_harness_support.cpp
  ${CMAKE_SOURCE_DIR}/src/domain/mesh/tests/support/mesh_test_harness.cpp
  ${CMAKE_SOURCE_DIR}/src/domain/mesh/tests/support/mesh_triple_harness.cpp)
target_link_libraries(pp_browser_feature_call_media_test PRIVATE
  pp_feature_call_media
  pp_domain_mesh
  pp_domain_people
  pp_foundation_identity
  pp_domain_net
  GTest::gtest
  GTest::gtest_main)
pp_browser_apply_warnings(pp_browser_feature_call_media_test)
pp_browser_add_gtest(pp_browser_feature_call_media_test)
