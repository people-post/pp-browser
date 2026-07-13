set(PP_BROWSER_CONFIG_TEST_SOURCES
  ${CMAKE_SOURCE_DIR}/src/feature/settings/SettingsLogic.cpp
)

set(PP_BROWSER_SETTINGS_SECTION_TEST_SOURCES
  ${PP_BROWSER_CONFIG_TEST_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/feature/settings/LlmSettingsSection.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/settings/IntegrationsSettingsSection.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/settings/NetworkSettingsSection.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/settings/AppearanceSettingsSection.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/settings/StorageSettingsSection.cpp
)

add_executable(pp_browser_config_merge_test
  config_merge_test.cpp
  ${PP_BROWSER_CONFIG_TEST_SOURCES})
target_compile_definitions(pp_browser_config_merge_test PRIVATE
  PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_link_libraries(pp_browser_config_merge_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_config_merge_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_config_merge_test)

add_executable(pp_browser_settings_sections_test
  settings_sections_test.cpp
  ${PP_BROWSER_SETTINGS_SECTION_TEST_SOURCES})
target_link_libraries(pp_browser_settings_sections_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_settings_sections_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_settings_sections_test)
