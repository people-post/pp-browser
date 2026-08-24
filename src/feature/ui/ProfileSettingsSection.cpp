#include "feature/ui/ProfileSettingsSection.h"

#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"

namespace pbr {

void ProfileSettingsSection::BindPorts(SettingsCommands* commands) {
  commands_ = commands;
}

void ProfileSettingsSection::ApplyIdentityView(const ProfileIdentityView& view, SettingsUiState& state) const {
  if (!view.ready) {
    return;
  }
  state.profile_nickname = view.nickname;
  state.profile_peer_id = view.peer_id;
  state.profile_relay_id = view.relay_id;
  state.profile_public_key = view.public_key_b64;
  state.profile_registered = view.registered;
  state.profile_registration_status = view.registration_status;
  state.profile_registration_expires = view.registration_expires;
  state.profile_register_label = view.register_label;
  state.profile_show_register = view.show_register;
  state.profile_show_rotate = view.show_rotate;
  state.brief_llm_key_masked = view.brief_llm_key_masked;
  state.profile_icon_src = view.profile_icon_path;
  state.profile_has_icon = view.profile_has_icon;
  state.profile_show_clear_icon = view.profile_has_icon && view.registered == "yes";
}

const char* ProfileSettingsSection::Id() const {
  return "profile";
}

SettingsSectionListItem ProfileSettingsSection::ListItem() const {
  return {.id = Id(), .title = Tr("settings.profile.title"), .subtitle = Tr("settings.profile.subtitle")};
}

SettingsFlushMode ProfileSettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void ProfileSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.auto_renew_registration = bootstrap.profile_prefs.auto_renew_registration ? "auto" : "off";
  state.show_notifications = bootstrap.profile_prefs.show_notifications ? "on" : "off";
  if (!commands_ || !commands_->load_profile_identity) {
    return;
  }
  ApplyIdentityView(commands_->load_profile_identity(), state);
}

bool ProfileSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const bool auto_renew = state.auto_renew_registration != "off";
  if (auto_renew != bootstrap.profile_prefs.auto_renew_registration) {
    return false;
  }
  const bool show_notifications = state.show_notifications != "off";
  if (show_notifications != bootstrap.profile_prefs.show_notifications) {
    return false;
  }
  if (!commands_ || !commands_->load_profile_identity) {
    return true;
  }
  const ProfileIdentityView view = commands_->load_profile_identity();
  if (!view.ready) {
    return true;
  }
  return state.profile_nickname == view.nickname;
}

Roe<void> ProfileSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  const bool auto_renew = state.auto_renew_registration != "off";
  const bool show_notifications = state.show_notifications != "off";
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  bool prefs_dirty = false;
  if (auto_renew != prefs.auto_renew_registration) {
    prefs.auto_renew_registration = auto_renew;
    prefs_dirty = true;
  }
  if (show_notifications != prefs.show_notifications) {
    prefs.show_notifications = show_notifications;
    prefs_dirty = true;
  }
  if (prefs_dirty) {
    prefs.schema_version = ProfilePreferences::kSchemaVersion;
    if (auto saved = store.SaveProfilePrefs(prefs); !saved) {
      return saved.error();
    }
  }

  if (commands_ && commands_->save_profile_nickname) {
    if (auto saved = commands_->save_profile_nickname(state.profile_nickname); !saved) {
      return saved.error();
    }
  }

  SyncFromSession(store.Snapshot(), state);
  return {};
}

void ProfileSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  SyncFromSession(store.Snapshot(), state);
}

} // namespace pbr
