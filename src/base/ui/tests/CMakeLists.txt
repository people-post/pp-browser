pp_browser_add_base_folder_tests(
  pp_base_ui
  pp_browser_ui_test
  LINK_LIBS
    nlohmann_json::nlohmann_json)

include(pp_lib_rmlui)
pp_browser_link_rmlui_core(pp_browser_ui_test)
