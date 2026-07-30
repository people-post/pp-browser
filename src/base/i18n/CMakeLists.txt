pp_browser_add_base_library(pp_base_i18n
  SOURCES
    LocalizationService.cpp
    ScriptLanguageDetector.cpp
  PUBLIC_LIBS
    pp_base_platform
    pp_base_data
    nlohmann_json::nlohmann_json)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
