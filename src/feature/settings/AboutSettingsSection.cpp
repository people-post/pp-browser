#include "feature/settings/AboutSettingsSection.h"

#include "base/i18n/LocalizationService.h"
#include "base/platform/AppVersion.h"
#include "base/platform/ProductBranding.h"

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
  return false;
}

void AboutSettingsSection::SyncFromSession(const BootstrapResult& /*bootstrap*/, SettingsUiState& state) {
  state.app_name = kProductName;
  state.app_version = AppVersionString();
}

bool AboutSettingsSection::IsPersisted(const SettingsUiState& /*state*/,
                                       const BootstrapResult& /*bootstrap*/) const {
  return true;
}

Roe<void> AboutSettingsSection::Flush(SettingsUiState& /*state*/, SessionStore& /*store*/) {
  return {};
}

void AboutSettingsSection::ResetToDefaults(SettingsUiState& /*state*/, const SessionStore& /*store*/) {}

} // namespace pbr
