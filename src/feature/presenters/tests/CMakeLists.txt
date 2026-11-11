# The presenters' decisions: the chats list rows and menus, group event text, mention spans, member rows.
pp_browser_add_feature_folder_tests(
  pp_feature_presenters
  pp_browser_feature_presenters_test
  LINK_LIBS pp_lib_delivery_testing pp_lib_calls_testing
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
