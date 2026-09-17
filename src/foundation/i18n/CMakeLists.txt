# Always platform_core: LocalizationService only needs AssetIO / IAssetLocator.
# Linking pp_foundation_platform here formerly pulled SDL/RmlUi into pp-node
# whenever the desktop GUI tree was configured (via pp_foundation_error).
pp_browser_add_foundation_library(pp_foundation_i18n
  SOURCES
    LocalizationService.cpp
    ScriptLanguageDetector.cpp
  PUBLIC_LIBS
    pp_foundation_platform_core
    pp_foundation_data)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
