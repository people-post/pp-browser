add_executable(pp_browser_config_json_test config_json_test.cpp)
target_include_directories(pp_browser_config_json_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_config_json_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_config_json_test)

add_executable(pp_browser_schema_version_test
  schema_version_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/data/SchemaVersion.cpp)
target_include_directories(pp_browser_schema_version_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_schema_version_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_schema_version_test)
