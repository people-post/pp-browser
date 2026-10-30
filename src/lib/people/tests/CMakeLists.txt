pp_browser_add_lib_folder_tests(
  pp_lib_people
  pp_browser_people_test)

# retired_keychain_backup_test reads the build's version (at-rest A011 reminder).
target_link_libraries(pp_browser_people_test PRIVATE pp_foundation_runtime_core)
