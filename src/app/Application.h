#pragma once

#include "feature/ai/AgentSessions.h"
#include "app/ChatShellBridge.h"
#include "app/ContactsShellBridge.h"
#include "app/PeoplePickerShellBridge.h"
#include "app/Bootstrap.h"
#include "app/ConfigApplyBridge.h"
#include "foundation/data/SessionStore.h"
#include "lib/net/ClientCompat.h"
#include "common/Error.h"
#include "common/Module.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/product/ProductStack.h"
#include "foundation/runtime/AppLifecycle.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

class FontEngineInterfaceHarfBuzz;

namespace ui {
class Context;
}

namespace pbr {

class OnboardingController;
class ConfigSecretsStore;
class BadgeAggregator;
class CallController;
class CallUiBackend;
class ClientCompatController;
class FlowCoordinator;
class InputCoordinator;
class ConversationsFacade;
class PinGateController;
class ProfileSecretsEngine;
class ProfileUnlockGate;
class SettingsController;
class ContactsController;
class QrScanController;
class ShellHost;
struct SettingsToolPorts;
class PeoplePickerController;
class EmojiPickerController;
class UiPerfTestController;
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

  ConversationsHub& Conversations();
  /** App-owned profile vault / DEK service (Bootstrap initializes it). */
  ProfileSecretsEngine& Secrets();
  SessionStore& Store() { return store_; }
  const SessionStore& Store() const { return store_; }

  /** Tear down messaging hub + profile secrets when initialized. */
  void ShutdownMessaging();
  /** Wipe active profile data dir and reinitialize secrets + hub (app-owned lifecycle). */
  Roe<void> ResetActiveProfile();
  /**
   * Wipe the active profile's chat data ({profile}/threads/: chats, outbox, peer sessions) and
   * reinitialize the hub; the account, keys, contacts and settings stay, and the vault stays open.
   */
  Roe<void> ResetChatData();

  static std::string AssetsPath(const std::string& relative);

private:
  bool InitializeUiHost(const char* window_title, int window_width, int window_height,
                        const BootstrapResult& bootstrap, ui::Context*& context);
  SettingsToolPorts WireSettings(ui::Context* context);
  void WireShellPresenters(const SettingsToolPorts& settings_tool_ports);
  void WireCalls();
  void WireUnlockPinAndFlow();
  void WireAgentAndConfig();
  bool MountPresenters(ui::Context* context);
  void WireHubLifecycle(ui::Context* context, const BootstrapResult& bootstrap);
  /** The hub's event callbacks; its Shutdown clears them, so a reset sets them again. */
  void WireHubCallbacks();
  /** UI demo app: write the fixed sample data once, then refresh lists and badges. */
  void SeedAndRefreshUiDemo();
  /**
   * Stop messaging behind the teardown quiesce, as quit does, but the process lives on. With
   * `with_secrets`, the profile secrets stop too (a profile reset re-creates them).
   */
  Roe<void> StopMessagingForReset(bool with_secrets);

  // --- PIN grace window (#21 phase 1, DEV/PLAN-pin-grace.md) ---
  /** The window the user chose, in minutes (0 = off). */
  int GraceMinutes() const;
  /** Gives secrets_ its grace entry for the current profile (after every secrets_->Initialize). */
  void AttachGrace();
  /**
   * The window's end moves to now + GraceMinutes(): only while the vault is open and only from the foreground's
   * paths (entering / leaving it, its timer, quit) — a push wake or a background sync never extends it.
   */
  void TouchGrace();
  /** Every minute while in the foreground (the app may be killed without a background event). */
  void StartGraceTimer();
  void StopGraceTimer();
  /** Both pieces deleted, the timer stopped (the engine's ForgetGrace cannot stop the app's timer). */
  void ForgetGrace();

  bool initialized_ = false;
  uint64_t grace_timer_ = 0;
  AppLifecycle::ListenerId grace_foreground_listener_ = 0;
  AppLifecycle::ListenerId grace_background_listener_ = 0;
  SessionStore store_;
  std::unique_ptr<ProfileSecretsEngine> secrets_;
  /** Typed API keys under the profile DEK (PB-034); registered with secrets_, serves store_. */
  std::unique_ptr<ConfigSecretsStore> config_secrets_;
  /** The hub with calls and broadcast riding it (feature-level composition). */
  std::unique_ptr<ProductStack> product_;
  /** product_'s hub (non-owning; null once the stack is gone). */
  ConversationsHub* messaging_ = nullptr;
  std::unique_ptr<ConversationsFacade> messaging_facade_;
  std::unique_ptr<ConfigApplyBridge> config_apply_;
  std::unique_ptr<ClientCompatController> client_compat_;
  std::optional<ClientCompatSupport> support_discovery_;
  std::unique_ptr<BadgeAggregator> badges_;
  std::unique_ptr<InputCoordinator> input_;
  std::unique_ptr<FlowCoordinator> flow_;
  std::unique_ptr<ShellHost> shell_;
  std::unique_ptr<ChatController> chat_;
  std::atomic<bool> peer_icons_refresh_posted_{false}; // a peer-icon refresh is queued on the UI thread
  std::unique_ptr<CallController> call_;
  std::unique_ptr<CallUiBackend> call_ui_;
  std::unique_ptr<SettingsController> settings_;
  std::unique_ptr<ContactsController> contacts_;
  std::unique_ptr<QrScanController> qr_scan_;
  std::unique_ptr<ContactsShellBridge> contacts_shell_bridge_;
  std::unique_ptr<ChatShellBridge> chat_shell_bridge_;
  std::unique_ptr<PeoplePickerShellBridge> people_picker_shell_bridge_;
  std::unique_ptr<PeoplePickerController> people_picker_;
  std::unique_ptr<EmojiPickerController> emoji_picker_;
  /** Developer builds only (kDevToolsBuild); null otherwise. */
  std::unique_ptr<UiPerfTestController> ui_perf_;
  std::unique_ptr<ProfileUnlockGate> unlock_gate_;
  std::unique_ptr<PinGateController> pin_gate_;
  std::unique_ptr<OnboardingController> onboarding_;
  std::optional<AgentSessions> agent_sessions_;
  std::unique_ptr<FontEngineInterfaceHarfBuzz> harfbuzz_font_engine_;
};

} // namespace pbr
