#include "feature/settings/AppearanceSettingsSection.h"

#include "base/data/SessionStore.h"

namespace pbr {

const char* AppearanceSettingsSection::Id() const {
  return "appearance";
}

SettingsSectionListItem AppearanceSettingsSection::ListItem() const {
  return {.id = Id(), .title = "Appearance", .subtitle = "Light, dark, or system theme"};
}

SettingsFlushMode AppearanceSettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

void AppearanceSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.appearance = bootstrap.profile_prefs.appearance;
}

bool AppearanceSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  return state.appearance == bootstrap.profile_prefs.appearance;
}

Roe<void> AppearanceSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences profile_prefs = store.Snapshot().profile_prefs;
  profile_prefs.appearance = state.appearance;
  if (auto saved = store.SaveProfilePrefs(profile_prefs); !saved) {
    return saved.error();
  }

  SyncFromSession(store.Snapshot(), state);
  return {};
}

void AppearanceSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  state.appearance = store.DefaultProfilePrefs().appearance;
}

} // namespace pbr
