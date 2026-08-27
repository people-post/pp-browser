pp_browser_add_feature_library(pp_feature_settings
  SOURCES
    AboutSettingsSection.cpp
    AppearanceSettingsSection.cpp
    IntegrationsSettingsSection.cpp
    LlmSettingsSection.cpp
    NetworkSettingsSection.cpp
    ReachabilityNudge.cpp
    SettingsLogic.cpp
    SettingsSections.cpp
    SettingsTools.cpp
    StorageSettingsSection.cpp
  PUBLIC_LIBS)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
