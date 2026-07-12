#include "feature/settings/SecuritySettingsSection.h"

#include "base/data/SessionStore.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

const char* SecuritySettingsSection::Id() const {
  return "security";
}

SettingsSectionListItem SecuritySettingsSection::ListItem() const {
  return {.id = Id(), .title = "Security", .subtitle = "PIN and key protection"};
}

SettingsFlushMode SecuritySettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

void SecuritySettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  if (!MessagingHub::Instance().IsInitialized() || !MessagingHub::Instance().HasVault()) {
    state.pin_protection_status = "Not set up";
    state.security_can_change_pin = false;
    return;
  }
  if (bootstrap.profile_prefs.pin_is_default) {
    state.pin_protection_status = "App default";
  } else {
    state.pin_protection_status = "Custom PIN";
  }
  state.security_can_change_pin = MessagingHub::Instance().AreSecretsReady();
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
