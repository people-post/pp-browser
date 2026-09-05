#pragma once

#include "gui/chat/ChatController.h"
#include "feature/conversations/ConversationsHub.h"
#include "gui/shell/ShellHost.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

/**
 * Composition-root fan-out: SessionStore disk DTOs → service nested slices → Apply.
 * Services never see ProfilePreferences / full AppConfig for hot-reload.
 */
class ConfigApplyBridge {
public:
  using AssetPathResolver = std::function<std::string(const std::string& relative)>;

  void Bind(ConversationsHub& messaging, SessionStore& store, ShellHost& shell, ChatController& chat,
            AssetPathResolver resolve_asset);
  /** Seed last-applied slices from live SessionStore (no Apply). Then install listeners. */
  void InstallListeners();

private:
  void OnConfig(const AppConfig& config);
  void OnProfilePrefs(const ProfilePreferences& prefs);
  void ApplyChrome(const ShellHost::ChromePrefs& next, const ShellHost::ChromePrefs* previous);

  ConversationsHub* messaging_ = nullptr;
  SessionStore* store_ = nullptr;
  ShellHost* shell_ = nullptr;
  ChatController* chat_ = nullptr;
  AssetPathResolver resolve_asset_;
  std::optional<ConversationsHub::NetworkConfig> last_network_;
  std::optional<ConversationsHub::PolicyPrefs> last_policy_;
  std::optional<ConversationsHub::NotificationPrefs> last_notifications_;
  std::optional<ShellHost::ChromePrefs> last_chrome_;
  std::optional<LocalizationService::Prefs> last_locale_;
  std::optional<ChatController::AgentConfig> last_agent_;
};

} // namespace pbr
