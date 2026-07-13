add_executable(pp_browser_desktop_path_provider_test desktop_path_provider_test.cpp)
target_link_libraries(pp_browser_desktop_path_provider_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
target_include_directories(pp_browser_desktop_path_provider_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_desktop_path_provider_test)

add_executable(pp_browser_platform_defaults_test platform_defaults_test.cpp)
target_link_libraries(pp_browser_platform_defaults_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_platform_defaults_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_platform_defaults_test)
