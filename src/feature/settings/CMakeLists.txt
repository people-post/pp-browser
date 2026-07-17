pp_browser_add_feature_library(pp_feature_settings
  SOURCES
    AppearanceSettingsSection.cpp
    IntegrationsSettingsSection.cpp
    LlmSettingsSection.cpp
    NetworkSettingsSection.cpp
    ProfileSettingsSection.cpp
    SecuritySettingsSection.cpp
    SettingsLogic.cpp
    SettingsSections.cpp
    StorageSettingsSection.cpp
  PUBLIC_LIBS
    nlohmann_json::nlohmann_json)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
