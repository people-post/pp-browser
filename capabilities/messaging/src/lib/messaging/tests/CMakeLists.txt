# Messaging's own tests: its ports filled by test doubles (TestAccount.h, the mock relay clients).
# data/group_roster_vectors.json: the group objects' frozen vectors (release-compatibility SIGNED_FORMATS § 2.5).
pp_browser_add_lib_folder_tests(
  pp_lib_messaging
  pp_browser_messaging_test
  PRIVATE_DEFINITIONS
    PP_GROUP_ROSTER_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/group_roster_vectors.json")
target_link_libraries(pp_browser_messaging_test PRIVATE pp_lib_delivery_testing pp_lib_messaging_testing)
