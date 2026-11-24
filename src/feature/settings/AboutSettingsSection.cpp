#include "feature/settings/AboutSettingsSection.h"

#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/product/AppVersion.h"
#include "foundation/product/BuildInfo.h"
#include "foundation/product/ProductBranding.h"
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
  return true; // only the "Show advanced settings" switch and the tester switch
}

void AboutSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.app_name = kProductName;
  state.app_version = AppVersionString();
  state.app_build = BuildLabel();
  state.tester_build_number = TesterBuildNumber(IsTesterBuild(), BuildTag(), AppVersionString());
  state.show_advanced_settings = bootstrap.profile_prefs.show_advanced_settings ? "on" : "off";
  state.show_tester_options = bootstrap.machine_prefs.show_tester_options ? "on" : "off";
}

bool AboutSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  return (state.show_advanced_settings == "on") == bootstrap.profile_prefs.show_advanced_settings &&
         (state.show_tester_options == "on") == bootstrap.machine_prefs.show_tester_options;
}

Roe<void> AboutSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences profile_prefs = store.Snapshot().profile_prefs;
  profile_prefs.schema_version = ProfilePreferences::kSchemaVersion;
  profile_prefs.show_advanced_settings = state.show_advanced_settings == "on";
  if (auto saved = store.SaveProfilePrefs(profile_prefs); !saved) {
    return saved.error();
  }
  // The tester switch is the device's (machine.json, profile-model PM006): read again from the file, as the other
  // writers of machine.json do, so their settings are kept.
  if (const bool tester = state.show_tester_options == "on";
      tester != store.Snapshot().machine_prefs.show_tester_options) {
    const std::string data_dir = store.Snapshot().data_dir;
    if (!data_dir.empty()) {
      auto machine = UserPreferences::LoadMachine(data_dir);
      MachinePreferences prefs = machine ? *machine : store.Snapshot().machine_prefs;
      prefs.show_tester_options = tester;
      if (auto saved = UserPreferences::SaveMachine(data_dir, prefs); !saved) {
        return saved.error();
      }
    }
    store.Mutable().machine_prefs.show_tester_options = tester;
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void AboutSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  state.show_advanced_settings = store.DefaultProfilePrefs().show_advanced_settings ? "on" : "off";
  state.show_tester_options = MachinePreferences{}.show_tester_options ? "on" : "off";
}

} // namespace pbr
