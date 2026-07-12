#include "feature/settings/ProfileSettingsSection.h"

#include "base/data/SessionStore.h"
#include "base/net/RegistrationClientUtil.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

const char* ProfileSettingsSection::Id() const {
  return "profile";
}

SettingsSectionListItem ProfileSettingsSection::ListItem() const {
  return {.id = Id(), .title = "Profile", .subtitle = "Identity and network registration"};
}

SettingsFlushMode ProfileSettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void ProfileSettingsSection::SyncFromSession(const BootstrapResult& /*bootstrap*/, SettingsUiState& state) {
  if (!MessagingHub::Instance().IsInitialized() || !MessagingHub::Instance().IsMessagingReady()) {
    return;
  }
  auto identity = MessagingHub::Instance().Identity().Get();
  if (!identity) {
    return;
  }
  state.profile_nickname = identity->nickname;
  state.profile_peer_id = identity->peer_id;
  state.profile_relay_id = identity->relay_user_id;
  state.profile_public_key = identity->public_key_b64;
  state.profile_registered = identity->registered ? "yes" : "no";
}

bool ProfileSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& /*bootstrap*/) const {
  if (!MessagingHub::Instance().IsInitialized()) {
    return true;
  }
  auto identity = MessagingHub::Instance().Identity().Get();
  if (!identity) {
    return true;
  }
  return state.profile_nickname == identity->nickname;
}

Roe<void> ProfileSettingsSection::Flush(SettingsUiState& state, SessionStore& /*store*/) {
  if (!MessagingHub::Instance().IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    return Error("Unlock profile PIN to save identity");
  }

  auto identity = MessagingHub::Instance().Identity().Get();
  if (!identity) {
    return identity.error();
  }

  if (state.profile_nickname == identity->nickname) {
    return {};
  }

  LocalIdentity updated = *identity;
  updated.nickname = state.profile_nickname;
  if (auto saved = MessagingHub::Instance().Identity().Update(updated); !saved) {
    return saved.error();
  }

  if (identity->registered) {
    auto result = UpdateRegisteredNickname(MessagingHub::Instance().Registration(), MessagingHub::Instance().Identity(),
                                           state.profile_nickname);
    if (!result) {
      return result.error();
    }
  }

  SyncFromSession(SessionStore::Instance().Snapshot(), state);
  return {};
}

void ProfileSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& /*store*/) {
  SyncFromSession(SessionStore::Instance().Snapshot(), state);
}

Roe<void> ProfileSettingsSection::RegisterIdentity(SettingsUiState& state) {
  if (!MessagingHub::Instance().IsInitialized()) {
    return Error("Messaging hub not initialized");
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    return Error("Unlock profile PIN to register");
  }

  auto identity = MessagingHub::Instance().Identity().Get();
  if (!identity) {
    return identity.error();
  }

  if (!state.profile_nickname.empty()) {
    LocalIdentity updated = *identity;
    updated.nickname = state.profile_nickname;
    if (auto saved = MessagingHub::Instance().Identity().Update(updated); !saved) {
      return saved.error();
    }
    identity = MessagingHub::Instance().Identity().Get();
    if (!identity) {
      return identity.error();
    }
  }

  auto result = FinishRegistrationWithIdentity(MessagingHub::Instance().Registration(),
                                             MessagingHub::Instance().Identity(), identity->nickname);
  if (!result) {
    return result.error();
  }

  LocalIdentity updated = *identity;
  updated.registered = result->success;
  if (!result->relay_user_id.empty()) {
    updated.relay_user_id = result->relay_user_id;
  }
  if (auto saved = MessagingHub::Instance().Identity().Update(updated); !saved) {
    return saved.error();
  }

  ProfileSettingsSection section;
  section.SyncFromSession(SessionStore::Instance().Snapshot(), state);
  return {};
}

} // namespace pbr
