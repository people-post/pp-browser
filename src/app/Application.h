#pragma once

#include "feature/ai/AgentSession.h"
#include "app/ChatShellBridge.h"
#include "app/ContactsShellBridge.h"
#include "app/PeoplePickerShellBridge.h"
#include "app/Bootstrap.h"
#include "app/ConfigApplyBridge.h"
#include "foundation/data/SessionStore.h"
#include "domain/net/ClientCompat.h"
#include "common/Error.h"
#include "common/Module.h"
#include "feature/messaging/MessagingHub.h"

#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

class FontEngineInterfaceHarfBuzz;

namespace pbr {

class ActionRouter;
class BadgeAggregator;
class CallController;
class CallUiBackend;
class ClientCompatController;
class FlowCoordinator;
class InputCoordinator;
class MessagingFacade;
class PinGateController;
class ProfileSecretsService;
class ProfileUnlockGate;
class SettingsController;
class ContactsController;
class ShellHost;
class PeoplePickerController;
class EmojiPickerController;
class ChatController;

class Application : public Module {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  bool Initialize(const char* window_title);
  void Run();
  void Shutdown();

  MessagingHub& Messaging();
  /** App-owned profile vault / DEK service (Bootstrap initializes it). */
  ProfileSecretsService& Secrets();
  SessionStore& Store() { return store_; }
  const SessionStore& Store() const { return store_; }

  /** Tear down messaging hub + profile secrets when initialized. */
  void ShutdownMessaging();
  /** Wipe active profile data dir and reinitialize secrets + hub (app-owned lifecycle). */
  Roe<void> ResetActiveProfile();

  static std::string AssetsPath(const std::string& relative);

private:
  bool initialized_ = false;
  SessionStore store_;
  std::unique_ptr<ProfileSecretsService> secrets_;
  std::unique_ptr<MessagingHub> messaging_;
  std::unique_ptr<MessagingFacade> messaging_facade_;
  std::unique_ptr<ConfigApplyBridge> config_apply_;
  std::unique_ptr<ActionRouter> action_router_;
  std::unique_ptr<ClientCompatController> client_compat_;
  std::optional<ClientCompatSupport> support_discovery_;
  std::unique_ptr<BadgeAggregator> badges_;
  std::unique_ptr<InputCoordinator> input_;
  std::unique_ptr<FlowCoordinator> flow_;
  std::unique_ptr<ShellHost> shell_;
  std::unique_ptr<ChatController> chat_;
  std::unique_ptr<CallController> call_;
  std::unique_ptr<CallUiBackend> call_ui_;
  std::unique_ptr<SettingsController> settings_;
  std::unique_ptr<ContactsController> contacts_;
  std::unique_ptr<ContactsShellBridge> contacts_shell_bridge_;
  std::unique_ptr<ChatShellBridge> chat_shell_bridge_;
  std::unique_ptr<PeoplePickerShellBridge> people_picker_shell_bridge_;
  std::unique_ptr<PeoplePickerController> people_picker_;
  std::unique_ptr<EmojiPickerController> emoji_picker_;
  std::unique_ptr<ProfileUnlockGate> unlock_gate_;
  std::unique_ptr<PinGateController> pin_gate_;
  std::optional<AgentSession> agent_session_;
  std::unique_ptr<FontEngineInterfaceHarfBuzz> harfbuzz_font_engine_;
};

} // namespace pbr
