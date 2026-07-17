pp_browser_add_feature_library(pp_feature_ai_bindings
  SOURCES
    ActionRouter.cpp
    BindingsManifest.cpp
  PUBLIC_LIBS
    pp_rmlui_backend
    RmlUi::Core
    nlohmann_json::nlohmann_json)
