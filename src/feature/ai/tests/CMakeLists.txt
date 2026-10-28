pp_browser_add_feature_folder_tests(
  pp_feature_ai
  pp_browser_feature_ai_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
  LINK_LIBS
    sodium)
