add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/ai/bindings/tests
  ${CMAKE_BINARY_DIR}/tests/feature_ai_bindings_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/ai/tests
  ${CMAKE_BINARY_DIR}/tests/feature_ai_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/chat/tests
  ${CMAKE_BINARY_DIR}/tests/feature_chat_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/ui/tests
  ${CMAKE_BINARY_DIR}/tests/feature_ui_tests)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/feature/settings/tests
  ${CMAKE_BINARY_DIR}/tests/feature_settings_tests)
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
add_subdirectory(${CMAKE_SOURCE_DIR}/src/base/platform/tests
  ${CMAKE_BINARY_DIR}/tests/base_platform_tests)

# Fork-level RmlUi click routing (not feature/base).
add_executable(pp_browser_click_routing_test click_routing_test.cpp)
target_link_libraries(pp_browser_click_routing_test PRIVATE
  GTest::gtest
  GTest::gtest_main)
pp_browser_link_rmlui_core(pp_browser_click_routing_test)
target_include_directories(pp_browser_click_routing_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/render/fork/Source/Core)
pp_browser_add_gtest(pp_browser_click_routing_test)
