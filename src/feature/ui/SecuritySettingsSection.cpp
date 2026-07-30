#include "feature/ui/SecuritySettingsSection.h"

#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"

namespace pbr {

std::string GroupInvitePolicyDisplayLabel(const std::string& policy) {
  if (policy == "everyone") {
    return Tr("settings.security.group_invites.everyone");
  }
  if (policy == "nobody") {
    return Tr("settings.security.group_invites.nobody");
  }
  return Tr("settings.security.group_invites.contacts_only");
}

void SecuritySettingsSection::BindPorts(SettingsCommands* commands) {
  commands_ = commands;
}

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
  state.group_invite_policy_label = GroupInvitePolicyDisplayLabel(state.group_invite_policy);

  PinProtectionView pin;
  if (commands_ && commands_->load_pin_protection) {
    pin = commands_->load_pin_protection();
  }
  if (!pin.ready) {
    state.pin_protection_status = "Not set up";
    state.security_can_change_pin = false;
    return;
  }
  if (bootstrap.profile_prefs.pin_is_default) {
    state.pin_protection_status = "App default";
  } else {
    state.pin_protection_status = "Custom PIN";
  }
  state.security_can_change_pin = pin.unlocked;
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
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void SecuritySettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  SyncFromSession(store.Snapshot(), state);
}

} // namespace pbr
