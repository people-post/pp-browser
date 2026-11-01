include(PpBrowserFeature)

# Acyclic module order: each library links only lower layers.
# Rml presenters live in src/gui/ (F008); feature stays Rml-free at the module edge. The GUI-free presenters
# (decisions and buttons, also driven by pp-app-probe) are the app's: feature/presenters (CR019).
add_subdirectory(settings)
add_subdirectory(ai)
add_subdirectory(broadcast)
add_subdirectory(product)
add_subdirectory(presenters)

set(PP_FEATURE_MODULE_LIBS
  pp_feature_settings
  pp_feature_ai
  pp_feature_calls
  pp_feature_broadcast
  pp_feature_conversations
  pp_feature_product
  pp_feature_presenters
)

add_library(pp_feature INTERFACE)
target_link_libraries(pp_feature INTERFACE
  ${PP_FEATURE_MODULE_LIBS}
  pp_domain
  pp_lib_ui
  ui::core
  CURL::libcurl
  Threads::Threads)
