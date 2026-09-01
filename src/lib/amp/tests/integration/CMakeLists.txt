if(NOT PP_BROWSER_BUILD_TESTS)
  return()
endif()

add_executable(pp_browser_amp_integration_test
  ${CMAKE_CURRENT_SOURCE_DIR}/amp_integration_test.cpp
  ${PP_AMP_TEST_SUPPORT_SOURCES})
target_include_directories(pp_browser_amp_integration_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_amp_integration_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base_mesh_link
  pp_crypto
  sodium
  pp_common)
pp_browser_apply_warnings(pp_browser_amp_integration_test)
pp_browser_add_gtest(pp_browser_amp_integration_test)
