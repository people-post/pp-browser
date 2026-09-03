pp_browser_add_feature_library(pp_feature_ai_bindings
  SOURCES
    ActionRouter.cpp
    BindingsManifest.cpp
  PUBLIC_LIBS
    pp_domain_ui
    RmlUi::Core)
