add_executable(pp_browser_bindings_test bindings_test.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_bindings_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_bindings_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME bindings_test COMMAND pp_browser_bindings_test)

add_executable(pp_browser_rml_validator_test rml_validator_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/RmlValidator.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_rml_validator_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_validator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rml_validator_test COMMAND pp_browser_rml_validator_test)

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
add_test(NAME mcp_mock_test COMMAND pp_browser_mcp_mock_test)

add_executable(pp_browser_structured_text_parser_test structured_text_parser_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_structured_text_parser_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_structured_text_parser_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME structured_text_parser_test COMMAND pp_browser_structured_text_parser_test)

add_executable(pp_browser_rml_mount_test rml_mount_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/RmlValidator.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_rml_mount_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_mount_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rml_mount_test COMMAND pp_browser_rml_mount_test)

add_executable(pp_browser_shell_host_test shell_host_test.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/ShellLayout.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/ShellInterruption.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/ShellFeedback.cpp)
target_include_directories(pp_browser_shell_host_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_shell_host_test PRIVATE RmlUi::Core)
add_test(NAME shell_host_test COMMAND pp_browser_shell_host_test)

add_executable(pp_browser_llm_client_test llm_client_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/LlmClient.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_llm_client_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_llm_client_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME llm_client_test COMMAND pp_browser_llm_client_test)

add_executable(pp_browser_web_search_test web_search_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/tools/WebSearchTool.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_web_search_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_web_search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME web_search_test COMMAND pp_browser_web_search_test)

add_executable(pp_browser_turn_plan_test turn_plan_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_turn_plan_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_plan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME turn_plan_test COMMAND pp_browser_turn_plan_test)

add_executable(pp_browser_payload_turn_plan_test payload_turn_plan_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PayloadTurnPlanBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_payload_turn_plan_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_payload_turn_plan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME payload_turn_plan_test COMMAND pp_browser_payload_turn_plan_test)

add_executable(pp_browser_turn_planner_test turn_planner_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_turn_planner_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_planner_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME turn_planner_test COMMAND pp_browser_turn_planner_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(pp_browser_turn_executor_test turn_executor_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnExecutor.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/ToolRegistry.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/ToolResultFormatter.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_turn_executor_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_turn_executor_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME turn_executor_test COMMAND pp_browser_turn_executor_test)

add_executable(pp_browser_output_repair_test output_repair_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_output_repair_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_output_repair_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME output_repair_test COMMAND pp_browser_output_repair_test)

add_executable(pp_browser_prompt_builder_test prompt_builder_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/TurnPlan.cpp)
target_link_libraries(pp_browser_prompt_builder_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_prompt_builder_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME prompt_builder_test COMMAND pp_browser_prompt_builder_test)

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
add_test(NAME sliding_window_context_policy_test COMMAND pp_browser_sliding_window_context_policy_test)

add_executable(pp_browser_chat_form_helper_test chat_form_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/ChatFormHelper.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/ChatWidgetStateBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/CalendarHelper.cpp)
target_link_libraries(pp_browser_chat_form_helper_test PRIVATE nlohmann_json::nlohmann_json RmlUi::Core)
target_include_directories(pp_browser_chat_form_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chat_form_helper_test COMMAND pp_browser_chat_form_helper_test)

add_executable(pp_browser_calendar_helper_test calendar_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/demo/CalendarHelper.cpp)
target_link_libraries(pp_browser_calendar_helper_test PRIVATE RmlUi::Core)
target_include_directories(pp_browser_calendar_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME calendar_helper_test COMMAND pp_browser_calendar_helper_test)

add_executable(pp_browser_chat_action_dispatcher_test chat_action_dispatcher_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/messaging/PeopleDiscoveryBlocks.cpp)
target_link_libraries(pp_browser_chat_action_dispatcher_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_chat_action_dispatcher_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chat_action_dispatcher_test COMMAND pp_browser_chat_action_dispatcher_test)

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
add_test(NAME messaging_foundation_test COMMAND pp_browser_messaging_foundation_test)
