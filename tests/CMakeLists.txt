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
target_link_libraries(pp_browser_mcp_mock_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_mcp_mock_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_sources(pp_browser_mcp_mock_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp
  ${CMAKE_SOURCE_DIR}/src/mcp/McpClient.cpp
  ${CMAKE_SOURCE_DIR}/src/mcp/SchemaAdapter.cpp
)
add_test(NAME mcp_mock_test COMMAND pp_browser_mcp_mock_test)

add_executable(pp_browser_structured_text_parser_test structured_text_parser_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp)
target_link_libraries(pp_browser_structured_text_parser_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_structured_text_parser_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME structured_text_parser_test COMMAND pp_browser_structured_text_parser_test)

add_executable(pp_browser_rml_mount_test rml_mount_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/RmlValidator.cpp
  ${CMAKE_SOURCE_DIR}/src/bindings/BindingsManifest.cpp)
target_link_libraries(pp_browser_rml_mount_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_mount_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rml_mount_test COMMAND pp_browser_rml_mount_test)

add_executable(pp_browser_split_tree_test split_tree_test.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/SplitTree.cpp)
target_include_directories(pp_browser_split_tree_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME split_tree_test COMMAND pp_browser_split_tree_test)

add_executable(pp_browser_llm_client_test llm_client_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/LlmClient.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_llm_client_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_llm_client_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME llm_client_test COMMAND pp_browser_llm_client_test)

add_executable(pp_browser_web_search_test web_search_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/tools/WebSearchTool.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/SearchIntent.cpp
  ${CMAKE_SOURCE_DIR}/src/common/Module.cpp
  ${CMAKE_SOURCE_DIR}/src/log/Logger.cpp)
target_link_libraries(pp_browser_web_search_test PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
target_include_directories(pp_browser_web_search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME web_search_test COMMAND pp_browser_web_search_test)

add_executable(pp_browser_search_intent_test search_intent_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/SearchIntent.cpp)
target_include_directories(pp_browser_search_intent_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME search_intent_test COMMAND pp_browser_search_intent_test)

add_executable(pp_browser_prompt_builder_test prompt_builder_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/PromptBuilder.cpp)
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
  ${CMAKE_SOURCE_DIR}/src/demo/ChatFormHelper.cpp)
target_link_libraries(pp_browser_chat_form_helper_test PRIVATE nlohmann_json::nlohmann_json RmlUi::Core)
target_include_directories(pp_browser_chat_form_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chat_form_helper_test COMMAND pp_browser_chat_form_helper_test)

add_executable(pp_browser_chat_action_dispatcher_test chat_action_dispatcher_test.cpp
  ${CMAKE_SOURCE_DIR}/src/agent/StructuredTextParser.cpp)
target_link_libraries(pp_browser_chat_action_dispatcher_test PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_chat_action_dispatcher_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chat_action_dispatcher_test COMMAND pp_browser_chat_action_dispatcher_test)
