function(pp_browser_add_test test_name executable_target)
  add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${executable_target}>)
  if(ARGN)
    set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${ARGN})
  endif()
endfunction()

add_executable(pp_browser_bindings_test bindings_test.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_bindings_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_bindings_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(bindings_test pp_browser_bindings_test)

add_executable(pp_browser_rml_validator_test rml_validator_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/RmlValidator.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_rml_validator_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_validator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(rml_validator_test pp_browser_rml_validator_test)

add_executable(pp_browser_mcp_mock_test mcp_mock_test.cpp)
target_link_libraries(pp_browser_mcp_mock_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_mcp_mock_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_sources(pp_browser_mcp_mock_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp
  ${CMAKE_SOURCE_DIR}/src/mcp/McpClient.cpp
  ${CMAKE_SOURCE_DIR}/src/mcp/SchemaAdapter.cpp
  ${CMAKE_SOURCE_DIR}/src/net/HttpClient.cpp
)
pp_browser_add_test(mcp_mock_test pp_browser_mcp_mock_test)

add_executable(pp_browser_structured_text_parser_test structured_text_parser_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/WorkingSetPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_structured_text_parser_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_structured_text_parser_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(structured_text_parser_test pp_browser_structured_text_parser_test)

add_executable(pp_browser_working_set_policy_test working_set_policy_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/WorkingSetPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_working_set_policy_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_working_set_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(working_set_policy_test pp_browser_working_set_policy_test)

add_executable(pp_browser_rml_mount_test rml_mount_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/RmlValidator.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_rml_mount_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_mount_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(rml_mount_test pp_browser_rml_mount_test)

add_executable(pp_browser_shell_host_test shell_host_test.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/ShellLayout.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/ShellInterruption.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/ShellFeedback.cpp)
target_include_directories(pp_browser_shell_host_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_link_rmlui_core(pp_browser_shell_host_test)
pp_browser_add_test(shell_host_test pp_browser_shell_host_test)

add_executable(pp_browser_llm_client_test llm_client_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/LlmClient.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_llm_client_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_llm_client_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(llm_client_test pp_browser_llm_client_test)

add_executable(pp_browser_web_search_test web_search_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/tools/WebSearchTool.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_web_search_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_web_search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(web_search_test pp_browser_web_search_test)

add_executable(pp_browser_turn_plan_test turn_plan_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_turn_plan_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_plan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(turn_plan_test pp_browser_turn_plan_test)

add_executable(pp_browser_payload_turn_plan_test payload_turn_plan_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PayloadTurnPlanBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_payload_turn_plan_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_payload_turn_plan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(payload_turn_plan_test pp_browser_payload_turn_plan_test)

add_executable(pp_browser_turn_planner_test turn_planner_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_turn_planner_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_planner_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(turn_planner_test pp_browser_turn_planner_test ${CMAKE_SOURCE_DIR})

add_executable(pp_browser_turn_executor_test turn_executor_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnExecutor.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/ToolRegistry.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/ToolResultFormatter.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/WorkingSetPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_turn_executor_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_executor_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(turn_executor_test pp_browser_turn_executor_test)

add_executable(pp_browser_output_repair_test output_repair_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_output_repair_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_output_repair_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(output_repair_test pp_browser_output_repair_test)

add_executable(pp_browser_prompt_builder_test prompt_builder_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_prompt_builder_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_prompt_builder_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(prompt_builder_test pp_browser_prompt_builder_test)

add_executable(pp_browser_sliding_window_context_policy_test sliding_window_context_policy_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/conversation/Conversation.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/conversation/SlidingWindowContextPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/conversation/TurnCoordinator.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/conversation/UserMessageFormatter.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/LlmClient.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_sliding_window_context_policy_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_sliding_window_context_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(sliding_window_context_policy_test pp_browser_sliding_window_context_policy_test)

add_executable(pp_browser_chat_form_helper_test chat_form_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/ChatFormHelper.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/ChatWidgetStateBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/CalendarHelper.cpp)
target_link_libraries(pp_browser_chat_form_helper_test PRIVATE nlohmann_json::nlohmann_json)
pp_browser_link_rmlui_core(pp_browser_chat_form_helper_test)
target_include_directories(pp_browser_chat_form_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(chat_form_helper_test pp_browser_chat_form_helper_test)

add_executable(pp_browser_calendar_helper_test calendar_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/CalendarHelper.cpp)
pp_browser_link_rmlui_core(pp_browser_calendar_helper_test)
target_include_directories(pp_browser_calendar_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(calendar_helper_test pp_browser_calendar_helper_test)

add_executable(pp_browser_click_routing_test click_routing_test.cpp)
pp_browser_link_rmlui_core(pp_browser_click_routing_test)
target_include_directories(pp_browser_click_routing_test PRIVATE ${CMAKE_SOURCE_DIR}/src/render/Source/Core)
pp_browser_add_test(click_routing_test pp_browser_click_routing_test)

add_executable(pp_browser_chat_action_dispatcher_test chat_action_dispatcher_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/WorkingSetPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_chat_action_dispatcher_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_chat_action_dispatcher_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(chat_action_dispatcher_test pp_browser_chat_action_dispatcher_test)

add_executable(pp_browser_messaging_foundation_test messaging_foundation_test.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/AtAiParser.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/JsonThreadStore.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/IdUtil.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp
  ${CMAKE_SOURCE_DIR}/src/identity/Ed25519Signer.cpp)
target_link_libraries(pp_browser_messaging_foundation_test PRIVATE nlohmann_json::nlohmann_json)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_browser_messaging_foundation_test PRIVATE OpenSSL::Crypto)
endif()
target_include_directories(pp_browser_messaging_foundation_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(messaging_foundation_test pp_browser_messaging_foundation_test)

set(PP_BROWSER_CONFIG_TEST_SOURCES
  ${CMAKE_SOURCE_DIR}/src/app/Config.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ConfigJson.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LlmPreset.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SettingsLogic.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SessionStore.cpp
  ${CMAKE_SOURCE_DIR}/src/app/AppPaths.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ProfileRegistry.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SchemaVersion.cpp
  ${CMAKE_SOURCE_DIR}/src/app/UserPreferences.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/Platform.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/IPathProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/DesktopPathProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/IAssetLocator.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/DesktopAssetLocator.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/PlatformDefaults.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/ICredentialStore.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/EnvCredentialStore.cpp
)

add_executable(pp_browser_desktop_path_provider_test desktop_path_provider_test.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/IPathProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/DesktopPathProvider.cpp)
target_include_directories(pp_browser_desktop_path_provider_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(desktop_path_provider_test pp_browser_desktop_path_provider_test)

add_executable(pp_browser_config_merge_test config_merge_test.cpp ${PP_BROWSER_CONFIG_TEST_SOURCES})
target_compile_definitions(pp_browser_config_merge_test PRIVATE PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_link_libraries(pp_browser_config_merge_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_config_merge_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(config_merge_test pp_browser_config_merge_test)

add_executable(pp_browser_session_store_test session_store_test.cpp ${PP_BROWSER_CONFIG_TEST_SOURCES})
target_link_libraries(pp_browser_session_store_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_session_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(session_store_test pp_browser_session_store_test)

add_executable(pp_browser_profile_registry_test profile_registry_test.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ProfileRegistry.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SchemaVersion.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_profile_registry_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_profile_registry_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(profile_registry_test pp_browser_profile_registry_test)

add_executable(pp_browser_schema_version_test schema_version_test.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SchemaVersion.cpp)
target_link_libraries(pp_browser_schema_version_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_schema_version_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(schema_version_test pp_browser_schema_version_test)

add_executable(pp_browser_platform_defaults_test platform_defaults_test.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/Platform.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/PlatformDefaults.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/ICredentialStore.cpp
  ${CMAKE_SOURCE_DIR}/src/platform/EnvCredentialStore.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_platform_defaults_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_platform_defaults_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_test(platform_defaults_test pp_browser_platform_defaults_test)
