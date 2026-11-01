# Messaging's own tests: its ports filled by test doubles (TestAccount.h, the mock relay clients).
pp_browser_add_lib_folder_tests(
  pp_lib_messaging
  pp_browser_messaging_test)
target_link_libraries(pp_browser_messaging_test PRIVATE pp_lib_messaging_testing)
