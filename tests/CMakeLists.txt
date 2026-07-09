add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/ai/bindings/tests
  ${CMAKE_BINARY_DIR}/tests/feature_ai_bindings_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/ai/tests
  ${CMAKE_BINARY_DIR}/tests/feature_ai_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/ai/tests
  ${CMAKE_BINARY_DIR}/tests/base_ai_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/ai/conversation/tests
  ${CMAKE_BINARY_DIR}/tests/base_ai_conversation_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/ai/mcp/tests
  ${CMAKE_BINARY_DIR}/tests/base_ai_mcp_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/messaging/tests
  ${CMAKE_BINARY_DIR}/tests/base_messaging_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/crypto/tests
  ${CMAKE_BINARY_DIR}/tests/base_crypto_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/data/tests
  ${CMAKE_BINARY_DIR}/tests/base_data_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/net/tests
  ${CMAKE_BINARY_DIR}/tests/base_net_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/people/tests
  ${CMAKE_BINARY_DIR}/tests/base_people_tests)

add_executable(pp_browser_rml_mount_test rml_mount_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/RmlValidator.cpp)
target_link_libraries(pp_browser_rml_mount_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_mount_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_rml_mount_test)

add_executable(pp_browser_shell_host_test shell_host_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ui/ShellLayout.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ui/ShellInterruption.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ui/ShellFeedback.cpp)
target_link_libraries(pp_browser_shell_host_test PRIVATE
  GTest::gtest
  GTest::gtest_main)
target_include_directories(pp_browser_shell_host_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_link_rmlui_core(pp_browser_shell_host_test)
pp_browser_add_gtest(pp_browser_shell_host_test)

add_executable(pp_browser_turn_planner_test turn_planner_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp)
target_link_libraries(pp_browser_turn_planner_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_planner_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(turn_planner_test pp_browser_turn_planner_test ${CMAKE_SOURCE_DIR})

add_executable(pp_browser_chat_form_helper_test chat_form_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/ChatFormHelper.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/ChatWidgetStateBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/CalendarHelper.cpp)
target_link_libraries(pp_browser_chat_form_helper_test PRIVATE nlohmann_json::nlohmann_json)
pp_browser_link_rmlui_core(pp_browser_chat_form_helper_test)
target_include_directories(pp_browser_chat_form_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(chat_form_helper_test pp_browser_chat_form_helper_test)

add_executable(pp_browser_calendar_helper_test calendar_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/CalendarHelper.cpp)
pp_browser_link_rmlui_core(pp_browser_calendar_helper_test)
target_include_directories(pp_browser_calendar_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(calendar_helper_test pp_browser_calendar_helper_test)

add_executable(pp_browser_click_routing_test click_routing_test.cpp)
pp_browser_link_rmlui_core(pp_browser_click_routing_test)
target_include_directories(pp_browser_click_routing_test PRIVATE ${CMAKE_SOURCE_DIR}/src/render/fork/Source/Core)
pp_browser_add_test(click_routing_test pp_browser_click_routing_test)

add_executable(pp_browser_chat_action_dispatcher_test chat_action_dispatcher_test.cpp)
target_link_libraries(pp_browser_chat_action_dispatcher_test PRIVATE pp_base nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_chat_action_dispatcher_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(chat_action_dispatcher_test pp_browser_chat_action_dispatcher_test)

add_executable(pp_browser_messaging_foundation_test messaging_foundation_test.cpp)
target_link_libraries(pp_browser_messaging_foundation_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_messaging_foundation_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_messaging_foundation_test)

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

add_executable(pp_browser_desktop_path_provider_test desktop_path_provider_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/IPathProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/DesktopPathProvider.cpp)
target_include_directories(pp_browser_desktop_path_provider_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(desktop_path_provider_test pp_browser_desktop_path_provider_test)

add_executable(pp_browser_config_merge_test config_merge_test.cpp ${PP_BROWSER_CONFIG_TEST_SOURCES})
target_compile_definitions(pp_browser_config_merge_test PRIVATE PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_link_libraries(pp_browser_config_merge_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_config_merge_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_config_merge_test)

add_executable(pp_browser_settings_sections_test settings_sections_test.cpp
  ${PP_BROWSER_SETTINGS_SECTION_TEST_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/base/data/AppPaths.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/IPathProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/DesktopPathProvider.cpp)
target_link_libraries(pp_browser_settings_sections_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_settings_sections_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_settings_sections_test)

add_executable(pp_browser_platform_defaults_test platform_defaults_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/Platform.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/PlatformDefaults.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/ICredentialStore.cpp
  ${CMAKE_SOURCE_DIR}/src/base/platform/EnvCredentialStore.cpp)
target_link_libraries(pp_browser_platform_defaults_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_common
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_platform_defaults_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_platform_defaults_test)

