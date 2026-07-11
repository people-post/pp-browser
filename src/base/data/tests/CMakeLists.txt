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
  ${CMAKE_SOURCE_DIR}/src/base/data/SchemaVersion.cpp
  ${CMAKE_SOURCE_DIR}/src/base/data/AtomicFileWrite.cpp)
target_include_directories(pp_browser_schema_version_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_schema_version_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_schema_version_test)

add_executable(pp_browser_profile_registry_test profile_registry_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/data/ProfileRegistry.cpp
  ${CMAKE_SOURCE_DIR}/src/base/data/SchemaVersion.cpp
  ${CMAKE_SOURCE_DIR}/src/base/data/AtomicFileWrite.cpp)
target_link_libraries(pp_browser_profile_registry_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_profile_registry_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_profile_registry_test)

add_executable(pp_browser_session_store_test session_store_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/settings/SettingsLogic.cpp)
target_link_libraries(pp_browser_session_store_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_session_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_session_store_test)

add_executable(pp_browser_atomic_file_write_test atomic_file_write_test.cpp)
target_link_libraries(pp_browser_atomic_file_write_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
target_include_directories(pp_browser_atomic_file_write_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_atomic_file_write_test)
