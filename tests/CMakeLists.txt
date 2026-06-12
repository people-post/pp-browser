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
