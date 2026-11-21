# data/account_locker_vectors.json: locker record v2's frozen vectors, read by the relay (SIGNED_FORMATS § 2.6).
pp_browser_add_lib_folder_tests(
  pp_lib_people
  pp_browser_people_test
  PRIVATE_DEFINITIONS
    PP_ACCOUNT_LOCKER_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/account_locker_vectors.json")

# retired_keychain_backup_test reads the build's version (at-rest A011 reminder).
target_link_libraries(pp_browser_people_test PRIVATE pp_foundation_runtime_core)
target_link_libraries(pp_browser_people_test PRIVATE pp_lib_people_testing)
