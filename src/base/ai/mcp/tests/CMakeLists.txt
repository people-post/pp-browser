add_executable(pp_browser_mcp_mock_test mcp_mock_test.cpp)
target_include_directories(pp_browser_mcp_mock_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_mcp_mock_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json
  CURL::libcurl)
pp_browser_add_gtest(pp_browser_mcp_mock_test)
