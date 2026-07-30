#include "feature/settings/AppearanceSettingsSection.h"

#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"

namespace pbr {

std::string ThemeDisplayLabel(const std::string& appearance_pref) {
  if (appearance_pref == "light") {
    return Tr("settings.theme.light");
  }
  if (appearance_pref == "dark") {
    return Tr("settings.theme.dark");
  }
  return Tr("settings.theme.system");
}

void AppearanceSettingsSection::BindPorts(SettingsCommands* commands) {
  commands_ = commands;
}

std::string AppearanceSettingsSection::ResolveLanguageLabel(const std::string& language_pref) const {
  if (commands_ && commands_->language_display_label) {
    return commands_->language_display_label(language_pref);
  }
  return language_pref;
}

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
  state.appearance_label = ThemeDisplayLabel(state.appearance);
  state.language = bootstrap.profile_prefs.language.empty() ? "system" : bootstrap.profile_prefs.language;
  state.language_label = ResolveLanguageLabel(state.language);
  state.reduce_transparency = bootstrap.profile_prefs.reduce_transparency ? "on" : "off";
}

bool AppearanceSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const bool reduce = state.reduce_transparency == "on";
  return state.appearance == bootstrap.profile_prefs.appearance &&
         state.language == bootstrap.profile_prefs.language &&
         reduce == bootstrap.profile_prefs.reduce_transparency;
}

Roe<void> AppearanceSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences profile_prefs = store.Snapshot().profile_prefs;
  profile_prefs.schema_version = ProfilePreferences::kSchemaVersion;
  profile_prefs.appearance = state.appearance;
  profile_prefs.language = state.language.empty() ? "system" : state.language;
  profile_prefs.reduce_transparency = state.reduce_transparency == "on";
  if (auto saved = store.SaveProfilePrefs(profile_prefs); !saved) {
    return saved.error();
  }

  SyncFromSession(store.Snapshot(), state);
  return {};
}

void AppearanceSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  const ProfilePreferences defaults = store.DefaultProfilePrefs();
  state.appearance = defaults.appearance;
  state.appearance_label = ThemeDisplayLabel(state.appearance);
  state.language = defaults.language;
  state.language_label = ResolveLanguageLabel(state.language);
  state.reduce_transparency = defaults.reduce_transparency ? "on" : "off";
}

} // namespace pbr
