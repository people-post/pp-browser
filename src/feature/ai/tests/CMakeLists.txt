add_executable(pp_browser_payload_turn_plan_test
  payload_turn_plan_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ai/PayloadTurnPlanBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp)

target_include_directories(pp_browser_payload_turn_plan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_payload_turn_plan_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)

pp_browser_add_gtest(pp_browser_payload_turn_plan_test)

add_executable(pp_browser_web_search_test
  web_search_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ai/tools/WebSearchTool.cpp)
target_include_directories(pp_browser_web_search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_web_search_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_common
  nlohmann_json::nlohmann_json
  CURL::libcurl)
pp_browser_add_gtest(pp_browser_web_search_test)

add_executable(pp_browser_turn_executor_test
  turn_executor_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ai/TurnExecutor.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ai/ToolRegistry.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/ToolResultFormatter.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/StructuredTextParser.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/WorkingSetPolicy.cpp
  ${CMAKE_SOURCE_DIR}/src/base/messaging/MessagingJson.cpp
  ${CMAKE_SOURCE_DIR}/src/base/messaging/PeopleDiscoveryBlocks.cpp
  ${CMAKE_SOURCE_DIR}/src/base/people/ContactJson.cpp
  ${CMAKE_SOURCE_DIR}/src/base/messaging/RelayStreamKey.cpp
  ${CMAKE_SOURCE_DIR}/src/base/crypto/CryptoUtil.cpp)
target_link_libraries(pp_browser_turn_executor_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_common
  nlohmann_json::nlohmann_json
  sodium)
target_include_directories(pp_browser_turn_executor_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_turn_executor_test)

