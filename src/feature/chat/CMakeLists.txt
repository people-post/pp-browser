pp_browser_add_feature_library(pp_feature_chat
  SOURCES
    CalendarHelper.cpp
    ChatController.cpp
    ChatWidgetStateBuilder.cpp
  PUBLIC_LIBS
    pp_rmlui_backend
    RmlUi::Core
    nlohmann_json::nlohmann_json)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
