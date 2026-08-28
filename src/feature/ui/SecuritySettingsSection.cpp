#include "feature/ui/SecuritySettingsSection.h"

#include "base/data/SessionStore.h"
#include "base/data/ToolPermissions.h"
#include "base/i18n/LocalizationService.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string ToolPermissionsSummaryLabel(const ToolPermissionsPrefs& perms) {
  const size_t count = RememberedToolPermissionCount(perms);
  if (count == 0) {
    return Tr("settings.security.tool_permissions.none");
  }
  return Tr("settings.security.tool_permissions.count") + " (" + std::to_string(count) + ")";
}

} // namespace

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
  state.tool_permissions_summary = ToolPermissionsSummaryLabel(bootstrap.profile_prefs.tool_permissions);
  state.tool_permissions_has_saved =
      RememberedToolPermissionCount(bootstrap.profile_prefs.tool_permissions) > 0;

  PinProtectionView pin;
  if (commands_ && commands_->load_pin_protection) {
    pin = commands_->load_pin_protection();
  }
  if (!pin.ready) {
    state.pin_protection_status = "Not set up";
    state.security_can_change_pin = false;
    state.security_can_export_link = false;
    return;
  }
  if (bootstrap.profile_prefs.pin_is_default) {
    state.pin_protection_status = "App default";
  } else {
    state.pin_protection_status = "Custom PIN";
  }
  state.security_can_change_pin = pin.unlocked;
  bool registered = false;
  if (commands_ && commands_->load_profile_identity) {
    registered = commands_->load_profile_identity().registered == "yes";
  }
  state.security_can_export_link = pin.unlocked && registered;
}

bool SecuritySettingsSection::IsPersisted(const SettingsUiState& state,
                                          const BootstrapResult& bootstrap) const {
  const bool invites_match = state.group_invite_policy == bootstrap.profile_prefs.group_invite_policy;
  const bool tools_match = state.tool_permissions_summary ==
                           ToolPermissionsSummaryLabel(bootstrap.profile_prefs.tool_permissions);
  return invites_match && tools_match;
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

Roe<void> SecuritySettingsSection::ResetToolPermissions(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  ClearToolPermissionDecisions(prefs.tool_permissions);
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  if (auto saved = store.SaveProfilePrefs(prefs); !saved) {
    return saved.error();
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

} // namespace pbr
