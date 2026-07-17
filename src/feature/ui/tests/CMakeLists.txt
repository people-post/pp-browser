pp_browser_add_feature_folder_tests(
  pp_feature_ui
  pp_browser_feature_ui_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

include(PpBrowserRender)
pp_browser_link_rmlui_core(pp_browser_feature_ui_test)
