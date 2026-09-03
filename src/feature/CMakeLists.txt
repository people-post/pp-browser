include(PpBrowserFeature)

# Acyclic module order: each library links only lower layers.
# Presenters live in src/gui/ (F008); feature stays Rml-free at the module edge.
add_subdirectory(settings)
add_subdirectory(ai)
add_subdirectory(messaging)

set(PP_FEATURE_MODULE_LIBS
  pp_feature_settings
  pp_feature_ai_tools
  pp_feature_ai_bindings
  pp_feature_ai
  pp_feature_messaging
)

add_library(pp_feature INTERFACE)
target_link_libraries(pp_feature INTERFACE
  ${PP_FEATURE_MODULE_LIBS}
  pp_domain_ui
  RmlUi::Core
  CURL::libcurl
  Threads::Threads)
