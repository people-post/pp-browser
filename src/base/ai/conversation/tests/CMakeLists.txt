add_executable(pp_browser_sliding_window_context_policy_test sliding_window_context_policy_test.cpp)
target_include_directories(pp_browser_sliding_window_context_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_sliding_window_context_policy_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json
  CURL::libcurl)
pp_browser_add_gtest(pp_browser_sliding_window_context_policy_test)

add_executable(pp_browser_thread_context_policy_test thread_context_policy_test.cpp)
target_include_directories(pp_browser_thread_context_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_thread_context_policy_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_thread_context_policy_test)
