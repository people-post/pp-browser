if(PP_BROWSER_HEADLESS)
  set(_pp_i18n_platform_lib pp_base_platform_core)
else()
  set(_pp_i18n_platform_lib pp_base_platform)
endif()

pp_browser_add_base_library(pp_base_i18n
  SOURCES
    LocalizationService.cpp
    ScriptLanguageDetector.cpp
  PUBLIC_LIBS
    ${_pp_i18n_platform_lib}
    pp_base_data)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
