#include "feature/settings/ProfileSettingsSection.h"

#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/error/AppError.h"
#include "base/net/HttpClient.h"
#include "base/net/RegistrationClientUtil.h"
#include "feature/chat/ChatController.h"
#include "feature/messaging/MessagingHub.h"

#include <nlohmann/json.hpp>

namespace pbr {

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

void SyncBriefKeyMasked(SettingsUiState& state) {
  state.brief_llm_key_masked.clear();
  if (!MessagingHub::Instance().IsInitialized() || !MessagingHub::Instance().IsMessagingReady()) {
    return;
  }
  auto identity = MessagingHub::Instance().Identity().Get();
  if (!identity) {
    return;
  }
  state.brief_llm_key_masked = MaskBriefLlmApiKey(identity->brief_llm_api_key);
}

void ReconfigureAgentIfNeeded() {
  ChatController::Instance().ReloadAgentConfig();
}

} // namespace

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
  SyncBriefKeyMasked(state);
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
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to save identity");
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
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to register");
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
  if (!result->llm_api_key.empty()) {
    updated.brief_llm_api_key = result->llm_api_key;
  }
  if (auto saved = MessagingHub::Instance().Identity().Update(updated); !saved) {
    return saved.error();
  }

  ProfileSettingsSection section;
  section.SyncFromSession(SessionStore::Instance().Snapshot(), state);
  ReconfigureAgentIfNeeded();
  return {};
}

Roe<void> ProfileSettingsSection::RotateBriefLlmKey(SettingsUiState& state) {
  if (!MessagingHub::Instance().IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to rotate API key");
  }

  auto identity = MessagingHub::Instance().Identity().Get();
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
    return AppError::Auth(Err::Auth::Forbidden,
                       "Brief key rotate HTTP " + std::to_string(response->status_code));
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
        .WithUser("Couldn't update Brief API key — try registering again in Me → Profile.");
  }
  const std::string new_key = root["llm_api_key"].get<std::string>();
  if (new_key.empty()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate returned empty key")
        .WithUser("Couldn't update Brief API key — try registering again in Me → Profile.");
  }

  LocalIdentity updated = *identity;
  updated.brief_llm_api_key = new_key;
  if (auto saved = MessagingHub::Instance().Identity().Update(updated); !saved) {
    return saved.error();
  }

  ProfileSettingsSection section;
  section.SyncFromSession(SessionStore::Instance().Snapshot(), state);
  ReconfigureAgentIfNeeded();
  return {};
}

} // namespace pbr
