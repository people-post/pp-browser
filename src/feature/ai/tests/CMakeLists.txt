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
