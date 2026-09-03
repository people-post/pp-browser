pp_browser_add_domain_folder_tests(
  pp_domain_ui
  pp_browser_ui_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

include(pp_lib_rmlui)
pp_browser_link_rmlui_core(pp_browser_ui_test)
