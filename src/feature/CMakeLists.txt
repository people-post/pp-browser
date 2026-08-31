include(PpBrowserFeature)

# Acyclic module order: each library links only lower layers.
add_subdirectory(settings)
add_subdirectory(ai)
add_subdirectory(messaging)
add_subdirectory(ui)
add_subdirectory(chat)

set(PP_FEATURE_MODULE_LIBS
  pp_feature_settings
  pp_feature_ai_tools
  pp_feature_ai_bindings
  pp_feature_ai
  pp_feature_messaging
  pp_feature_ui
  pp_feature_chat
)

add_library(pp_feature INTERFACE)
target_link_libraries(pp_feature INTERFACE
  ${PP_FEATURE_MODULE_LIBS}
  pp_base_render
  RmlUi::Core
  CURL::libcurl
  Threads::Threads)
