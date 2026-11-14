#include "gui/SecuritySettingsSection.h"

#include "foundation/crypto/CryptoConstants.h"

#include "foundation/data/SessionStore.h"
#include "foundation/data/ToolPermissions.h"
#include "foundation/i18n/LocalizationService.h"
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

std::string CallInvitePolicyDisplayLabel(const std::string& policy) {
  if (policy == "contacts_only") {
    return Tr("settings.security.calls.contacts_only");
  }
  if (policy == "nobody") {
    return Tr("settings.security.calls.nobody");
  }
  return Tr("settings.security.calls.everyone");
}

std::string UnlockGraceDisplayLabel(const int minutes) {
  if (minutes == 0) {
    return Tr("settings.security.grace.now");
  }
  if (minutes == 5) {
    return Tr("settings.security.grace.5");
  }
  if (minutes == 60) {
    return Tr("settings.security.grace.60");
  }
  if (GraceIsAlways(minutes)) {
    return Tr("settings.security.grace.always");
  }
  return Tr("settings.security.grace.15");
}

std::vector<int> OfferedUnlockGraceMinutes() {
  return {0, 5, 15, 60};
}

std::string BiometricSwitchLabel(const std::string& kind) {
  if (kind == "touch") {
    return Tr("settings.security.biometric.touch");
  }
  if (kind == "fingerprint") {
    return Tr("settings.security.biometric.fingerprint");
  }
  return Tr("settings.security.biometric.face");
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
  state.call_invite_policy = bootstrap.profile_prefs.call_invite_policy;
  state.call_invite_policy_label = CallInvitePolicyDisplayLabel(state.call_invite_policy);
  state.crash_reports_enabled = bootstrap.profile_prefs.crash_reports_enabled ? "on" : "off";
  state.tool_permissions_summary = ToolPermissionsSummaryLabel(bootstrap.profile_prefs.tool_permissions);
  state.tool_permissions_has_saved = RememberedToolPermissionCount(bootstrap.profile_prefs.tool_permissions) > 0;
  state.unlock_grace_minutes = bootstrap.profile_prefs.unlock_grace_minutes;
  state.unlock_grace_label = UnlockGraceDisplayLabel(state.unlock_grace_minutes);

  PinProtectionView pin;
  if (commands_ && commands_->load_pin_protection) {
    pin = commands_->load_pin_protection();
  }
  if (!pin.ready) {
    state.pin_protection_status = Tr("settings.security.pin_status.not_setup");
    state.pin_protection_is_default = false;
    state.pin_protection_not_setup = true;
    state.security_can_change_pin = false;
    state.security_can_export_link = false;
    state.security_has_recovery_phrase = false;
    state.security_grace_shown = false;
    state.security_biometric_shown = false;
    state.security_biometric_on = false;
    return;
  }
  if (bootstrap.profile_prefs.pin_is_default) {
    state.pin_protection_status = Tr("settings.security.pin_status.app_default");
  } else {
    state.pin_protection_status = Tr("settings.security.pin_status.custom");
  }
  state.pin_protection_is_default = bootstrap.profile_prefs.pin_is_default;
  state.pin_protection_not_setup = false;
  state.security_can_change_pin = pin.unlocked;
  bool registered = false;
  if (commands_ && commands_->load_profile_identity) {
    registered = commands_->load_profile_identity().registered == "yes";
  }
  state.security_can_export_link = pin.unlocked && registered;
  state.security_has_recovery_phrase =
      pin.unlocked && commands_ && commands_->has_recovery_phrase && commands_->has_recovery_phrase();
  // The app default PIN opens silently anyway; a platform with no secure store keeps no grace window.
  state.security_grace_shown = !bootstrap.profile_prefs.pin_is_default && commands_ && commands_->grace_supported &&
                               commands_->grace_supported();
  // Face / fingerprint unlock (A014): a device with the sensor, an own PIN (the app default opens silently anyway).
  const std::string biometric_kind =
      commands_ && commands_->biometric_kind ? commands_->biometric_kind() : std::string();
  state.security_biometric_shown = !biometric_kind.empty() && !bootstrap.profile_prefs.pin_is_default;
  state.security_biometric_on =
      state.security_biometric_shown && commands_->biometric_enabled && commands_->biometric_enabled();
  state.security_biometric_label = BiometricSwitchLabel(biometric_kind);
}

bool SecuritySettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const bool invites_match = state.group_invite_policy == bootstrap.profile_prefs.group_invite_policy &&
                             state.call_invite_policy == bootstrap.profile_prefs.call_invite_policy;
  const bool crash_match = (state.crash_reports_enabled == "on") == bootstrap.profile_prefs.crash_reports_enabled;
  const bool tools_match =
      state.tool_permissions_summary == ToolPermissionsSummaryLabel(bootstrap.profile_prefs.tool_permissions);
  const bool grace_match = state.unlock_grace_minutes == bootstrap.profile_prefs.unlock_grace_minutes;
  return invites_match && crash_match && tools_match && grace_match;
}

Roe<void> SecuritySettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  const bool crash_enabled = state.crash_reports_enabled == "on";
  const bool invites_changed = state.group_invite_policy != prefs.group_invite_policy;
  const bool calls_changed = state.call_invite_policy != prefs.call_invite_policy;
  const bool crash_changed = crash_enabled != prefs.crash_reports_enabled;
  const bool grace_changed = state.unlock_grace_minutes != prefs.unlock_grace_minutes;
  if (!invites_changed && !calls_changed && !crash_changed && !grace_changed) {
    return {};
  }
  if (invites_changed) {
    prefs.group_invite_policy = state.group_invite_policy;
  }
  if (calls_changed) {
    prefs.call_invite_policy = state.call_invite_policy;
  }
  if (crash_changed) {
    prefs.crash_reports_enabled = crash_enabled;
  }
  if (grace_changed) {
    prefs.unlock_grace_minutes = state.unlock_grace_minutes;
  }
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  if (auto saved = store.SaveProfilePrefs(prefs); !saved) {
    return saved.error();
  }
  // Saved first: the window then follows the new length (0 forgets it now; otherwise it restarts at that length).
  if (grace_changed && commands_ && commands_->set_grace_minutes) {
    commands_->set_grace_minutes(prefs.unlock_grace_minutes);
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
