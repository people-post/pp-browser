pp_browser_add_feature_folder_tests(
  pp_gui
  pp_browser_gui_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

include(pp_lib_rmlui)
pp_browser_link_rmlui_core(pp_browser_gui_test)
