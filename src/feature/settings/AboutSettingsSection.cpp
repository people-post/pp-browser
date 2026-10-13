#include "feature/settings/AboutSettingsSection.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppVersion.h"
#include "foundation/runtime/ProductBranding.h"
#include "common/PbrCompat.h"

namespace pbr {

const char* AboutSettingsSection::Id() const {
  return "about";
}

SettingsSectionListItem AboutSettingsSection::ListItem() const {
  return {.id = Id(), .title = Tr("settings.about.title"), .subtitle = Tr("settings.about.subtitle")};
}

SettingsFlushMode AboutSettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

bool AboutSettingsSection::IsWritable() const {
  return true; // only the "Show advanced settings" switch
}

void AboutSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.app_name = kProductName;
  state.app_version = AppVersionString();
  state.show_advanced_settings = bootstrap.profile_prefs.show_advanced_settings ? "on" : "off";
}

bool AboutSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  return (state.show_advanced_settings == "on") == bootstrap.profile_prefs.show_advanced_settings;
}

Roe<void> AboutSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences profile_prefs = store.Snapshot().profile_prefs;
  profile_prefs.schema_version = ProfilePreferences::kSchemaVersion;
  profile_prefs.show_advanced_settings = state.show_advanced_settings == "on";
  if (auto saved = store.SaveProfilePrefs(profile_prefs); !saved) {
    return saved.error();
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void AboutSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  state.show_advanced_settings = store.DefaultProfilePrefs().show_advanced_settings ? "on" : "off";
}

} // namespace pbr
