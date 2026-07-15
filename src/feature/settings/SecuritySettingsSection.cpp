#include "feature/settings/SecuritySettingsSection.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"

namespace pbr {

const char* SecuritySettingsSection::Id() const {
  return "security";
}

SettingsSectionListItem SecuritySettingsSection::ListItem() const {
  return {.id = Id(), .title = Tr("settings.security.title"), .subtitle = Tr("settings.security.subtitle")};
}

SettingsFlushMode SecuritySettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

void SecuritySettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  if (!ProfileSecretsService::Instance().IsInitialized() || !ProfileSecretsService::Instance().HasVault()) {
    state.pin_protection_status = "Not set up";
    state.security_can_change_pin = false;
    return;
  }
  if (bootstrap.profile_prefs.pin_is_default) {
    state.pin_protection_status = "App default";
  } else {
    state.pin_protection_status = "Custom PIN";
  }
  state.security_can_change_pin = ProfileSecretsService::Instance().IsUnlocked();
}

bool SecuritySettingsSection::IsPersisted(const SettingsUiState& /*state*/,
                                          const BootstrapResult& /*bootstrap*/) const {
  return true;
}

Roe<void> SecuritySettingsSection::Flush(SettingsUiState& /*state*/, SessionStore& /*store*/) {
  return {};
}

void SecuritySettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  SyncFromSession(store.Snapshot(), state);
}

} // namespace pbr
