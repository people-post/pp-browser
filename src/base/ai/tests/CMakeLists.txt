add_executable(pp_browser_turn_plan_test
  turn_plan_test.cpp
  ${CMAKE_SOURCE_DIR}/src/base/ai/TurnPlan.cpp)

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
