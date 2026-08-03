if(NOT PP_BROWSER_BUILD_TESTS)
  return()
endif()

add_executable(pp_browser_common_test worker_pool_test.cpp)
target_include_directories(pp_browser_common_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_common_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_common)
pp_browser_add_gtest(pp_browser_common_test)
