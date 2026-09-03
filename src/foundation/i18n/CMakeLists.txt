if(PP_BROWSER_HEADLESS)
  set(_pp_i18n_platform_lib pp_foundation_platform_core)
else()
  set(_pp_i18n_platform_lib pp_foundation_platform)
endif()

pp_browser_add_foundation_library(pp_foundation_i18n
  SOURCES
    LocalizationService.cpp
    ScriptLanguageDetector.cpp
  PUBLIC_LIBS
    ${_pp_i18n_platform_lib}
    pp_foundation_data)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
