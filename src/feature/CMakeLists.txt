include(PpBrowserFeature)

add_subdirectory(settings)
add_subdirectory(messaging)
add_subdirectory(ai)
add_subdirectory(chat)
add_subdirectory(ui)

set(PP_FEATURE_MODULE_LIBS
  pp_feature_settings
  pp_feature_messaging
  pp_feature_ai_bindings
  pp_feature_ai_tools
  pp_feature_ai
  pp_feature_chat
  pp_feature_ui
)

add_library(pp_feature INTERFACE)
target_link_libraries(pp_feature INTERFACE
  "$<LINK_GROUP:RESCAN,${PP_FEATURE_MODULE_LIBS}>"
  pp_rmlui_backend
  RmlUi::Core
  p2p
  nlohmann_json::nlohmann_json
  CURL::libcurl
  Threads::Threads)
