add_executable(pp_browser_shell_host_test
  shell_host_test.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ui/ShellLayout.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ui/ShellInterruption.cpp
  ${CMAKE_SOURCE_DIR}/src/feature/ui/ShellFeedback.cpp)
target_link_libraries(pp_browser_shell_host_test PRIVATE
  GTest::gtest
  GTest::gtest_main)
target_include_directories(pp_browser_shell_host_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
pp_browser_link_rmlui_core(pp_browser_shell_host_test)
pp_browser_add_gtest(pp_browser_shell_host_test)
