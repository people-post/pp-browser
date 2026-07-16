#include "feature/settings/SecuritySettingsSection.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "base/messaging/GroupTypes.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

const char* SecuritySettingsSection::Id() const {
  return "security";
}

SettingsSectionListItem SecuritySettingsSection::ListItem() const {
  return {.id = Id(), .title = Tr("settings.security.title"), .subtitle = Tr("settings.security.subtitle")};
}

SettingsFlushMode SecuritySettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void SecuritySettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.group_invite_policy = bootstrap.profile_prefs.group_invite_policy;
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

bool SecuritySettingsSection::IsPersisted(const SettingsUiState& state,
                                          const BootstrapResult& bootstrap) const {
  return state.group_invite_policy == bootstrap.profile_prefs.group_invite_policy;
}

Roe<void> SecuritySettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  if (state.group_invite_policy == prefs.group_invite_policy) {
    return {};
  }
  prefs.group_invite_policy = state.group_invite_policy;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  if (auto saved = store.SaveProfilePrefs(prefs); !saved) {
    return saved.error();
  }
  if (MessagingHub::Instance().IsInitialized() && MessagingHub::Instance().IsMessagingReady()) {
    const GroupInvitePolicy policy = GroupInvitePolicyFromString(prefs.group_invite_policy);
    MessagingHub::Instance().Groups().SetInboundPolicy(policy);
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void SecuritySettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  SyncFromSession(store.Snapshot(), state);
}

} // namespace pbr
