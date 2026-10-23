# Product shell peer (theme, catalogs, input, chat widget types).
# Window host / RmlUi Backend / overlays live in foundation/platform/ui/.

pp_browser_add_lib_library(pp_lib_ui
  SOURCES
    CalendarHelper.cpp
    CallConflictCopy.cpp
    ChatFormHelper.cpp
    ChatWidgetConfigBuilders.cpp
    ContextMenuHost.cpp
    EmojiCatalog.cpp
    InputCoordinator.cpp
    QrSvg.cpp
    RmlVariantHelpers.cpp
    ShellInterruption.cpp
    ShellLayout.cpp
    Theme.cpp
    UiEditSession.cpp
    ViewCatalog.cpp
  PUBLIC_LIBS
    pp_foundation_platform
    pp_foundation_i18n
    ui::core)

target_link_libraries(pp_lib_ui PRIVATE qrcodegen)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
