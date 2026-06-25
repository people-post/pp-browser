#include "feature/settings/StorageSettingsSection.h"

#include "base/data/AppPaths.h"

namespace pbr {

const char* StorageSettingsSection::Id() const {
  return "storage";
}

SettingsSectionListItem StorageSettingsSection::ListItem() const {
  return {.id = Id(), .title = "Storage", .subtitle = "Profile and data paths"};
}

SettingsFlushMode StorageSettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

bool StorageSettingsSection::IsWritable() const {
  return false;
}

void StorageSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.profile_label = bootstrap.profile_registry.ActiveProfileId();
  state.config_dir = AppPaths::ConfigDir();
  state.data_dir = bootstrap.data_dir;
  state.profile_dir = bootstrap.profile_data_dir;
}

bool StorageSettingsSection::IsPersisted(const SettingsUiState& /*state*/, const BootstrapResult& /*bootstrap*/) const {
  return true;
}

Roe<void> StorageSettingsSection::Flush(SettingsUiState& /*state*/, SessionStore& /*store*/) {
  return {};
}

void StorageSettingsSection::ResetToDefaults(SettingsUiState& /*state*/, const SessionStore& /*store*/) {}

} // namespace pbr
