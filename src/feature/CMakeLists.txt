include(PpBrowserFeature)

# Acyclic module order: each library links only lower layers.
# End-state names: messaging≈conversations; ui absorbs chat (+ shell/contacts bands).
add_subdirectory(settings)
add_subdirectory(ai)
add_subdirectory(messaging)
add_subdirectory(ui)

set(PP_FEATURE_MODULE_LIBS
  pp_feature_settings
  pp_feature_ai_tools
  pp_feature_ai_bindings
  pp_feature_ai
  pp_feature_messaging
  pp_feature_ui
)

add_library(pp_feature INTERFACE)
target_link_libraries(pp_feature INTERFACE
  ${PP_FEATURE_MODULE_LIBS}
  pp_domain_ui
  RmlUi::Core
  CURL::libcurl
  Threads::Threads)
