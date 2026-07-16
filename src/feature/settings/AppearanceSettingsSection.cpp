#include "feature/settings/AppearanceSettingsSection.h"

#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"

namespace pbr {

const char* AppearanceSettingsSection::Id() const {
  return "appearance";
}

SettingsSectionListItem AppearanceSettingsSection::ListItem() const {
  return {.id = Id(),
          .title = Tr("settings.appearance.title"),
          .subtitle = Tr("settings.appearance.subtitle")};
}

SettingsFlushMode AppearanceSettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

void AppearanceSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.appearance = bootstrap.profile_prefs.appearance;
  state.language = bootstrap.profile_prefs.language.empty() ? "system" : bootstrap.profile_prefs.language;
  state.language_label = LocalizationService::Instance().LanguageDisplayLabel(state.language);
}

bool AppearanceSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  return state.appearance == bootstrap.profile_prefs.appearance &&
         state.language == bootstrap.profile_prefs.language;
}

Roe<void> AppearanceSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences profile_prefs = store.Snapshot().profile_prefs;
  profile_prefs.schema_version = ProfilePreferences::kSchemaVersion;
  profile_prefs.appearance = state.appearance;
  profile_prefs.language = state.language.empty() ? "system" : state.language;
  if (auto saved = store.SaveProfilePrefs(profile_prefs); !saved) {
    return saved.error();
  }

  SyncFromSession(store.Snapshot(), state);
  return {};
}

void AppearanceSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  const ProfilePreferences defaults = store.DefaultProfilePrefs();
  state.appearance = defaults.appearance;
  state.language = defaults.language;
  state.language_label = LocalizationService::Instance().LanguageDisplayLabel(state.language);
}

} // namespace pbr
