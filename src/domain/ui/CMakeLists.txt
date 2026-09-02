# Product shell peer (theme, catalogs, input, chat widget types).
# Window host / RmlUi Backend / overlays live in foundation/platform/ui/.

pp_browser_add_domain_library(pp_domain_ui
  SOURCES
    ChatFormHelper.cpp
    ContextMenuHost.cpp
    EmojiCatalog.cpp
    InputCoordinator.cpp
    RmlVariantHelpers.cpp
    Theme.cpp
    ViewCatalog.cpp
  PUBLIC_LIBS
    pp_foundation_platform
    pp_foundation_i18n
    RmlUi::Core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
