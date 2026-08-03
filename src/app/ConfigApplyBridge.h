#pragma once

#include "feature/chat/ChatController.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/ShellHost.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"

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

  void Bind(MessagingHub& messaging, SessionStore& store, ShellHost& shell, ChatController& chat,
            AssetPathResolver resolve_asset);
  /** Seed last-applied slices from live SessionStore (no Apply). Then install listeners. */
  void InstallListeners();

private:
  void OnConfig(const AppConfig& config);
  void OnProfilePrefs(const ProfilePreferences& prefs);
  void ApplyChrome(const ShellHost::ChromePrefs& next, const ShellHost::ChromePrefs* previous);

  MessagingHub* messaging_ = nullptr;
  SessionStore* store_ = nullptr;
  ShellHost* shell_ = nullptr;
  ChatController* chat_ = nullptr;
  AssetPathResolver resolve_asset_;
  std::optional<MessagingHub::NetworkConfig> last_network_;
  std::optional<MessagingHub::PolicyPrefs> last_policy_;
  std::optional<MessagingHub::NotificationPrefs> last_notifications_;
  std::optional<ShellHost::ChromePrefs> last_chrome_;
  std::optional<LocalizationService::Prefs> last_locale_;
  std::optional<ChatController::AgentConfig> last_agent_;
};

} // namespace pbr
