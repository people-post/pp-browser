#include <stdexcept>
#include "feature/ui/ProfileSettingsSection.h"

#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/error/AppError.h"
#include "base/i18n/LocalizationService.h"
#include "base/net/HttpClient.h"
#include "base/net/RegistrationClientUtil.h"
#include "feature/ui/ChatSessionActions.h"
#include "feature/messaging/MessagingHub.h"

#include <nlohmann/json.hpp>

namespace pbr {

void ProfileSettingsSection::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
}

MessagingHub& ProfileSettingsSection::Hub() {
  if (!messaging_) {
    throw std::runtime_error("ProfileSettingsSection messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& ProfileSettingsSection::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("ProfileSettingsSection messaging not bound");
  }
  return *messaging_;
}

namespace {

std::string MaskBriefLlmApiKey(const std::string& key) {
  constexpr const char kPrefix[] = "brf_llm_";
  if (key.empty()) {
    return "";
  }
  if (key.size() <= sizeof(kPrefix) - 1 + 4) {
    return std::string(kPrefix) + "••••";
  }
  return key.substr(0, sizeof(kPrefix) - 1 + 4) + "••••";
}

std::string FormatExpiresDisplay(const std::string& iso) {
  if (iso.size() >= 10) {
    return iso.substr(0, 10);
  }
  return iso;
}

void SyncBriefKeyMasked(SettingsUiState& state, MessagingHub& messaging) {
  state.brief_llm_key_masked.clear();
  if (!messaging.IsInitialized() || !messaging.IsMessagingReady()) {
    return;
  }
  auto identity = messaging.Identity().Get();
  if (!identity) {
    return;
  }
  state.brief_llm_key_masked = MaskBriefLlmApiKey(identity->brief_llm_api_key);
}

void SyncRegistrationUi(SettingsUiState& state, const LocalIdentity& identity) {
  const RegistrationStatus status = ClassifyRegistration(identity);
  state.profile_registered = identity.registered ? "yes" : "no";
  state.profile_registration_status = RegistrationStatusLabel(status);
  state.profile_registration_expires =
      identity.registration_expires_at.empty() ? "" : FormatExpiresDisplay(identity.registration_expires_at);
  state.profile_register_label = RegistrationActionLabel(status);
  state.profile_show_register = true;
  state.profile_show_rotate = (status == RegistrationStatus::Active || status == RegistrationStatus::ExpiringSoon) &&
                              !identity.brief_llm_api_key.empty();
}

void ReconfigureAgentIfNeeded() {
  if (ChatSessionActions::Instance().reload_agent_config) {
    ChatSessionActions::Instance().reload_agent_config();
  }
}

} // namespace

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
  if (!Hub().IsInitialized() || !Hub().IsMessagingReady()) {
    return;
  }
  auto identity = Hub().Identity().Get();
  if (!identity) {
    return;
  }
  state.profile_nickname = identity->nickname;
  state.profile_peer_id = identity->peer_id;
  state.profile_relay_id = identity->relay_user_id;
  state.profile_public_key = identity->public_key_b64;
  SyncRegistrationUi(state, *identity);
  SyncBriefKeyMasked(state, Hub());
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
  if (!Hub().IsInitialized()) {
    return true;
  }
  auto identity = const_cast<MessagingHub&>(Hub()).Identity().Get();
  if (!identity) {
    return true;
  }
  return state.profile_nickname == identity->nickname;
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

  if (!Hub().IsInitialized()) {
    SyncFromSession(store.Snapshot(), state);
    return {};
  }

  auto identity = Hub().Identity().Get();
  if (!identity) {
    if (!Hub().IsMessagingReady()) {
      SyncFromSession(store.Snapshot(), state);
      return {};
    }
    return identity.error();
  }

  if (state.profile_nickname == identity->nickname) {
    SyncFromSession(store.Snapshot(), state);
    return {};
  }

  if (!Hub().IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to save identity");
  }

  LocalIdentity updated = *identity;
  updated.nickname = state.profile_nickname;
  if (auto saved = Hub().Identity().Update(updated); !saved) {
    return saved.error();
  }

  if (identity->registered) {
    auto result = UpdateRegisteredNickname(Hub().Registration(), Hub().Identity(),
                                           state.profile_nickname);
    if (!result) {
      return result.error();
    }
  }

  SyncFromSession(store.Snapshot(), state);
  return {};
}

void ProfileSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& /*store*/) {
  SyncFromSession(SessionStore::Instance().Snapshot(), state);
}

Roe<void> ProfileSettingsSection::RegisterIdentity(SettingsUiState& state, MessagingHub& messaging) {
  if (!messaging.IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!messaging.IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to register");
  }

  auto identity = messaging.Identity().Get();
  if (!identity) {
    return identity.error();
  }

  if (!state.profile_nickname.empty()) {
    LocalIdentity updated = *identity;
    updated.nickname = state.profile_nickname;
    if (auto saved = messaging.Identity().Update(updated); !saved) {
      return saved.error();
    }
    identity = messaging.Identity().Get();
    if (!identity) {
      return identity.error();
    }
  }

  auto applied = FinishAndPersistRegistration(messaging.Registration(),
                                              messaging.Identity(), identity->nickname);
  if (!applied) {
    return applied.error();
  }

  ProfileSettingsSection section;
  section.BindMessaging(messaging);
  section.SyncFromSession(SessionStore::Instance().Snapshot(), state);
  ReconfigureAgentIfNeeded();
  return {};
}

Roe<void> ProfileSettingsSection::RotateBriefLlmKey(SettingsUiState& state, MessagingHub& messaging) {
  if (!messaging.IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!messaging.IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to rotate API key");
  }

  auto identity = messaging.Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (identity->brief_llm_api_key.empty()) {
    return AppError::Auth(Err::Auth::NotRegistered, "No Brief API key yet")
        .WithUser("No Brief API key yet — register on the network in Me first.");
  }

  const AppConfig& config = SessionStore::Instance().Snapshot().config;
  std::string base_url = config.llm.base_url;
  if (ResolvePreset(config) != "brief" || base_url.empty()) {
    base_url = "https://www.brief.global/api/llm/v1";
  }
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  const std::string url = base_url + "/keys/rotate";

  auto response = HttpClient::Post(url, "{}", {{"Authorization", "Bearer " + identity->brief_llm_api_key},
                                               {"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response->status_code == 401 || response->status_code == 403) {
    LocalIdentity expired = *identity;
    MarkRegistrationExpired(expired);
    (void)messaging.Identity().Update(expired);
    ProfileSettingsSection section;
    section.SyncFromSession(SessionStore::Instance().Snapshot(), state);
    const char* renew_hint = response->status_code == 403
                                 ? "Registration expired — use Renew registration in Me → Profile."
                                 : "Brief API key rejected — renew registration in Me → Profile.";
    return AppError::Auth(Err::Auth::Forbidden,
                          "Brief key rotate HTTP " + std::to_string(response->status_code))
        .WithUser(renew_hint);
  }
  if (response->status_code < 200 || response->status_code >= 300) {
    auto json = nlohmann::json::parse(response->body, nullptr, false);
    std::string detail = "Brief API key rotate failed (HTTP " + std::to_string(response->status_code) + ")";
    if (!json.is_discarded() && json.contains("error")) {
      const auto& err = json["error"];
      if (err.is_string()) {
        detail = err.get<std::string>();
      } else if (err.is_object() && err.contains("message") && err["message"].is_string()) {
        detail = err["message"].get<std::string>();
      }
    }
    return AppError::Network(Err::Network::HttpError, detail);
  }

  auto root = nlohmann::json::parse(response->body, nullptr, false);
  if (root.is_discarded() || !root.contains("llm_api_key") || !root["llm_api_key"].is_string()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate response missing llm_api_key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }
  const std::string new_key = root["llm_api_key"].get<std::string>();
  if (new_key.empty()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate returned empty key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }

  LocalIdentity updated = *identity;
  updated.brief_llm_api_key = new_key;
  if (auto saved = messaging.Identity().Update(updated); !saved) {
    return saved.error();
  }

  ProfileSettingsSection section;
  section.BindMessaging(messaging);
  section.SyncFromSession(SessionStore::Instance().Snapshot(), state);
  ReconfigureAgentIfNeeded();
  return {};
}

} // namespace pbr
