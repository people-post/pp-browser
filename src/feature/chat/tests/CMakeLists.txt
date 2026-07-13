add_executable(pp_browser_calendar_helper_test
  calendar_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/CalendarHelper.cpp)
target_link_libraries(pp_browser_calendar_helper_test PRIVATE
  GTest::gtest
  GTest::gtest_main)
pp_browser_link_rmlui_core(pp_browser_calendar_helper_test)
target_include_directories(pp_browser_calendar_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_calendar_helper_test)

add_executable(pp_browser_chat_form_helper_test
  chat_form_helper_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/ChatFormHelper.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/ChatWidgetStateBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/chat/CalendarHelper.cpp)
target_link_libraries(pp_browser_chat_form_helper_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  nlohmann_json::nlohmann_json)
pp_browser_link_rmlui_core(pp_browser_chat_form_helper_test)
target_include_directories(pp_browser_chat_form_helper_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_add_gtest(pp_browser_chat_form_helper_test)
