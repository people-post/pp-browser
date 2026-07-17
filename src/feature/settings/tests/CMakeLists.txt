pp_browser_add_feature_folder_tests(
  pp_feature_settings
  pp_browser_feature_settings_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
  LINK_LIBS
    nlohmann_json::nlohmann_json)
