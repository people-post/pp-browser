# Messaging's own tests: no other lib peer (its ports filled by test doubles, TestAccount.h).
pp_browser_add_lib_folder_tests(
  pp_lib_messaging
  pp_browser_messaging_test)
