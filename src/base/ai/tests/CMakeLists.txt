add_executable(pp_browser_turn_plan_test
  turn_plan_test.cpp
  turn_planner_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp)

target_compile_definitions(pp_browser_turn_plan_test PRIVATE
  PP_BROWSER_TURN_PLAN_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/turn_plans")
target_include_directories(pp_browser_turn_plan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_turn_plan_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)

pp_browser_add_gtest(pp_browser_turn_plan_test)

add_executable(pp_browser_prompt_builder_test
  prompt_builder_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp)
target_include_directories(pp_browser_prompt_builder_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_prompt_builder_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_prompt_builder_test)

add_executable(pp_browser_output_repair_test
  output_repair_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/PromptBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp)
target_include_directories(pp_browser_output_repair_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_output_repair_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_output_repair_test)

add_executable(pp_browser_working_set_policy_test working_set_policy_test.cpp)
target_include_directories(pp_browser_working_set_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_working_set_policy_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_working_set_policy_test)

add_executable(pp_browser_structured_text_parser_test
  structured_text_parser_test.cpp
  chat_action_dispatcher_test.cpp)
target_include_directories(pp_browser_structured_text_parser_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_structured_text_parser_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_structured_text_parser_test)

add_executable(pp_browser_llm_client_test llm_client_test.cpp)
target_include_directories(pp_browser_llm_client_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_llm_client_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json
  CURL::libcurl)
pp_browser_add_gtest(pp_browser_llm_client_test)

add_executable(pp_browser_rml_validator_test
  rml_validator_test.cpp
  rml_mount_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/RmlValidator.cpp)
target_link_libraries(pp_browser_rml_validator_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
target_include_directories(pp_browser_rml_validator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_rml_validator_test)
