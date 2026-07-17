pp_browser_add_feature_folder_tests(
  pp_feature_chat
  pp_browser_feature_chat_test
  LINK_LIBS
    nlohmann_json::nlohmann_json)

include(PpBrowserRender)
pp_browser_link_rmlui_core(pp_browser_feature_chat_test)
