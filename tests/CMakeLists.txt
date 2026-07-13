# Cross-cutting / fork-level tests only. Module unit tests are registered from
# src/base and src/feature via pp_browser_register_tests().

# Fork-level RmlUi click routing (not feature/base).
add_executable(pp_browser_click_routing_test click_routing_test.cpp)
target_link_libraries(pp_browser_click_routing_test PRIVATE
  GTest::gtest
  GTest::gtest_main)
pp_browser_link_rmlui_core(pp_browser_click_routing_test)
target_include_directories(pp_browser_click_routing_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/render/fork/Source/Core)
pp_browser_add_gtest(pp_browser_click_routing_test)
