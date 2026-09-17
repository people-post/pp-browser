# LocalizationService needs AssetIO / IAssetLocator only — stay on platform_core so
# i18n does not depend on data (band order: platform → i18n/error → data).
pp_browser_add_foundation_library(pp_foundation_i18n
  SOURCES
    LocalizationService.cpp
    ScriptLanguageDetector.cpp
  PUBLIC_LIBS
    pp_foundation_platform_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
