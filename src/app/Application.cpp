#include "app/Application.h"
#include "foundation/platform/ui/PlatformNavigation.h"
#include "foundation/platform/CallSystemUi.h"
#include "foundation/platform/PendingNotificationTap.h"
#include "common/ValueJson.h"
#include <map>
#include "foundation/platform/IncomingLink.h"
#include "foundation/platform/os/OsThreadName.h"
#include "app/ConfigApplyBridge.h"
#include "app/DiagnosticsUpload.h"
#include "feature/product/DemoDataSeeder.h"
#include "feature/product/HubSettingsProjection.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/ProfileIconImagePrep.h"
#include "foundation/platform/PlatformAppBadge.h"
#include "foundation/platform/PlatformUserHints.h"
#include "common/Localized.h"
#include "lib/net/BriefGuestLlmClient.h"
#include "lib/net/HttpClient.h"
#include "domain/registration/BriefLlmKeyRenewal.h"

#include "foundation/data/ConfigSecretsStore.h"
#include "gui/onboarding/OnboardingController.h"
#include "foundation/platform/os/OsThreadStackDump.h"
#include "foundation/platform/PlatformShare.h"
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdlib>
#include <thread>
#include <utility>
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "foundation/crypto/PinRules.h"
#include "foundation/data/AppPaths.h"
#include "foundation/data/ProfileManifest.h"
#include "foundation/platform/OsSecureBlobStore.h"
#include "foundation/platform/os/OsProtectedData.h"
#include "foundation/platform/os/OsDebugFlag.h"
#include "foundation/platform/os/OsAppState.h"
#include "foundation/crypto/GraceUnlock.h"
#include "foundation/crypto/BiometricUnlock.h"
#include "foundation/platform/os/OsBiometry.h"
#include "common/Diag.h"
#include "lib/people/RetiredKeychainBackup.h"
#include "foundation/data/SessionStore.h"
#include "foundation/error/AppError.h"
#include "common/Error.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/delivery/ChatPayloadValidator.h"
#include "lib/storage/SqliteCallSessionStore.h"
#include "gui/ChatStoreRecovery.h"
#include "lib/storage/SqliteChatDbLadders.h"
#include "lib/storage/SqliteChatStoreGate.h"
#include "lib/storage/SqliteThreadSchema.h"
#include "common/chat/MessagingLimits.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/product/ProductBranding.h"
#include "lib/ui/InputCoordinator.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/ViewCatalog.h"
#include "gui/chat/ChatController.h"
#include "feature/product/MessagingTools.h"
#include "feature/settings/SettingsTools.h"
#include "common/runtime/AppRuntime.h"
#include "feature/product/ProductOwners.h"
#include "foundation/platform/IAssetLocator.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/IPathProvider.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/PlatformHooks.h"
#include "foundation/platform/AppEventHooks.h"
#include "foundation/platform/MobileWindowSizing.h"
#include "foundation/platform/NativeFileDialog.h"
#include "foundation/platform/ui/FrameDrawPolicy.h"
#include "foundation/platform/ui/SdlAppEvents.h"
#include "foundation/platform/StayConnectedService.h"
#include "foundation/platform/WindowIcon.h"
#include "feature/ai/AgentUiPorts.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/product/MakeCallFunctionalPorts.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/settings/OwnDevices.h"
#include "feature/settings/CasLibraryCommands.h"
#include "feature/settings/ReachabilityNudge.h"
#include "feature/settings/SettingsCommands.h"
#include "gui/BadgeNotifyPorts.h"
#include "gui/UiFrameStats.h"
#include "gui/CallActionsPorts.h"
#include "gui/ChatSessionPorts.h"
#include "app/ChatShellBridge.h"
#include "app/ContactsShellBridge.h"
#include "app/PeoplePickerShellBridge.h"
#include "gui/contacts/ContactsController.h"
#include "gui/avatar/AvatarCropController.h"
#include "gui/avatar/AvatarViewController.h"
#include "gui/avatar/ElementAvatarCrop.h"
#include "gui/scan/QrScanController.h"
#include "gui/scan/ElementScanPreview.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/ChatSurfaceNotifyPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/contacts/PeoplePickerSurfaceNotifyPorts.h"
#include "gui/PinGateActionPorts.h"
#include "gui/shell/ShellChromeApplyPorts.h"
#include "gui/CallController.h"
#include "gui/BadgeAggregator.h"
#include "gui/ClientCompatController.h"
#include "gui/SupportDiscoveryPorts.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/DeferredStartup.h"
#include "gui/FlowCoordinator.h"
#include "gui/PinGateController.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/UnlockGateCompletePorts.h"
#include "gui/contacts/PeoplePickerController.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/EmojiPickerController.h"
#include "gui/UiPerfTestController.h"
#include "foundation/product/DevTools.h"
#include "gui/EmojiPickerNotifyPorts.h"
#include "gui/SettingsController.h"
#include "app/StorageManagerBridge.h"
#include "gui/shell/ShellFeedback.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellHost.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/UserFeedback.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/SupportContact.h"
#include "common/Utilities.h"
#include "feature/conversations/MessagingCompatPorts.h"
#include "feature/conversations/MessagingContactsPorts.h"
#include "feature/conversations/MessagingShellPorts.h"
#include "feature/product/MessagingPeoplePickerPorts.h"
#include "feature/conversations/MessagingUiPorts.h"
#include "gui/shell/ShellCallChromePorts.h"
#include "gui/shell/ShellPinGatePorts.h"
#include "ElementCallVideoTile.h"
#include "lib/ui/Theme.h"
#include "common/StartupTiming.h"
#include "common/Metrics.h"
#include "mesh/reachability/Reachability.h"

#include <ui/dom/Context.h>
#include <ui/Core.h>
#include <ui/dom/ElementDocument.h>
#include <ui/base/Input.h>
#include <ui/base/TextLoupe.h>

#include "foundation/platform/ui/RmlUi_Backend.h"
#include <ui/render/Renderer_GL3.h>
#include "TextLoupeRenderer.h"
#include "FontEngineInterfaceHarfBuzz.h"

#ifdef PPBROWSER_ENABLE_DEBUGGER
#include <ui/Debugger.h>
#endif

#include <algorithm>
#include <filesystem>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

/** The Brief key store over the facade: every write is one modify under the identity store's lock. */
BriefLlmKeyStore MakeBriefLlmKeyStore(ConversationsFacade& facade) {
  return BriefLlmKeyStore{
      .get = [&facade]() { return facade.GetIdentity(); },
      .modify = [&facade](const std::function<bool(LocalIdentity&)>& change) -> Roe<void> {
        if (auto changed = facade.ModifyLocalIdentity(change); !changed) {
          return changed.error();
        }
        return {};
      },
      .mint = [](const std::string& llm_base_url) { return MintBriefGuestLlmKey(llm_base_url); },
  };
}

} // namespace

namespace {

/** A saved copy of the account: a sealed snapshot (at most 1 MiB + 256 bytes) as base64 inside JSON. */
constexpr size_t kMaxSavedAccountCopyBytes = 2 * 1024 * 1024;

/** Runtime teardown quiesce budgets (THREADING.md § Teardown quiesce). Quit must stay inside
 * AppRuntime::kShutdownDeadlineBudget (3 s watchdog); reset can wait for a slow relay call. */
constexpr std::chrono::milliseconds kQuitQuiesceBudget{2000};
constexpr std::chrono::milliseconds kQuitMediaDevicesBudget{1000};
constexpr std::chrono::milliseconds kProfileResetQuiesceBudget{10000};

InputCoordinator* g_input_coordinator = nullptr;

int64_t MsSinceStartup() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - StartupEpoch())
      .count();
}

/** PP_DEBUG_CALL_TAP (a development aid, see CONFIGURATION.md): the delay in ms, or -1 when unset / not a number. */
int64_t DebugCallTapDelayMs() {
  const char* value = std::getenv("PP_DEBUG_CALL_TAP");
  if (value == nullptr || value[0] == '\0') {
    return -1;
  }
  char* end = nullptr;
  const long long ms = std::strtoll(value, &end, 10);
  return (end == value || *end != '\0' || ms < 0) ? -1 : static_cast<int64_t>(ms);
}

/** What a tapped call alert records, without a push: for the iPhone USB build, which gets no pushes. */
void RecordDebugCallTap() {
  PendingNotificationTap::Instance().Record({}, /*call_wake=*/true, {}, PendingNotificationTap::Clock::now());
}

void WireShellPresentationEvents(ShellHost& shell, BadgeAggregator* badges, SettingsController& settings,
                                 ContactsController& contacts, ChatController& chat, UiPerfTestController* ui_perf) {
  shell.SetOnNavTabChanged([badges, &shell, &settings, &contacts, &chat](NavTab tab) {
    static NavTab previous = NavTab::Home;
    chat.CancelVoice(); // a recording is not carried to another tab, and a clip stops
    if (previous == NavTab::Me && tab != NavTab::Me) {
      settings.OnMeSurfaceClosed();
    }
    // Back from a phone hidden tab (Me, Contacts) to a chat overlay left open under it: the chat is still the one
    // shown, so Home / Chats are not reset to their lists (DEV/PLAN-minimal-nav.md A).
    const bool back_to_open_chat = shell.State().compact_chat_open;
    if (tab == NavTab::Home && !back_to_open_chat) {
      chat.OnHomeTabActivated();
    }
    if (tab == NavTab::Sessions && !back_to_open_chat) {
      chat.OnSessionsTabActivated();
    }
    chat.SyncDraftOwner(); // every tab change: the composer shows that place's own unsent text
    if (tab == NavTab::Contacts) {
      contacts.OnNavTabActivated();
    }
    if (tab == NavTab::Me) {
      settings.OnNavTabActivated();
    }
    previous = tab;
    if (badges) {
      badges->Refresh();
    }
    shell.DirtyNavChrome();
  });

  shell.SetOnCompactChatShownAgain([&chat]() { chat.OnCompactChatShownAgain(); });

  shell.SetOnLayoutModeChanged([&shell, &settings, &contacts, &chat](LayoutMode mode) {
    const ShellChromeSnapshot chrome = ProjectShellChromeSnapshot(shell.State());
    if (chrome.nav_tab == NavTab::Contacts) {
      contacts.SyncLayoutMode();
    }
    settings.SyncLayoutMode();
    if (mode == LayoutMode::Compact && chrome.nav_tab == NavTab::Home) {
      chat.OnHomeTabActivated();
    }
  });

  shell.SetOnLayoutSynced([&settings, &chat]() {
    settings.OnShellLayoutSynced();
    chat.OnShellLayoutSynced();
  });

  shell.SetOnSyncLayoutMeasured([&chat](int64_t ms) { chat.OnSyncLayoutMeasured(ms); });

  shell.SetOnSessionSwipeAction([&chat](const std::string& thread_id, SessionSwipeAction action) {
    chat.OnSessionSwipeAction(thread_id, action);
  });

  shell.SetOnNewChatSwipeAction([&contacts](const std::string& contact_id, const std::string& action) {
    contacts.OnNewChatSwipeAction(contact_id, action);
  });

  shell.SetOnTransientMounted([ui_perf](const std::string& key) {
    if (ui_perf && key == "ui_perf_test") {
      ui_perf->OnMounted();
    }
  });
  shell.SetOnTransientPopped([&settings, &contacts, ui_perf](const std::string& key) {
    if (ui_perf && key == "ui_perf_test") {
      ui_perf->OnClosed();
    }
    if (key == "contact_detail") {
      contacts.OnDetailDismissed();
    }
    if (key == "new_chat") {
      contacts.OnNewChatDismissed();
    }
    if (key == "add_contact") {
      contacts.OnAddContactDismissed();
    }
    if (key == "blocked_people") {
      contacts.OnBlockedPeopleDismissed();
      settings.RefreshBlockedCount();
    }
    if (key == "settings_detail") {
      settings.OnDetailDismissed();
    }
  });
}

bool ProcessKeyDown(ui::Context* context, ui::Input::KeyIdentifier key, int key_modifier, float /*native_dp_ratio*/,
                    bool priority) {
  if (!g_input_coordinator) {
    return true;
  }
  return g_input_coordinator->ProcessKeyDown(context, key, key_modifier, priority);
}

void ApplyUiDocumentLanguage(ui::Context* context) {
  if (!context || context->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* document = context->GetDocument(0);
  if (!document) {
    return;
  }
  document->SetAttribute("lang", LocalizationService::Instance().ResolvedLanguage().c_str());
}

/**
 * Runs `task` on the UI thread and waits for its result: assistant tools run on the worker pool, while shell
 * navigation and support_discovery_ are UI-thread state. A UI thread that does not get to it in time (busy,
 * shutting down) reads as "unavailable".
 */
Roe<void> RunOnUiAndWait(const std::function<Roe<void>()>& task) {
  if (!task) {
    return Error("unavailable");
  }
  if (AppRuntime::CurrentlyOnUI()) {
    return task();
  }
  struct Slot {
    std::mutex mu;
    std::condition_variable cv;
    std::optional<Roe<void>> result;
  };
  auto slot = std::make_shared<Slot>();
  AppRuntime::PostUI([slot, task]() {
    Roe<void> result = task();
    {
      std::lock_guard lock(slot->mu);
      slot->result.emplace(std::move(result));
    }
    slot->cv.notify_all();
  });
  std::unique_lock lock(slot->mu);
  if (!slot->cv.wait_for(lock, std::chrono::seconds(5), [&]() { return slot->result.has_value(); })) {
    return Error("UI thread did not run the task in time");
  }
  return std::move(*slot->result);
}

} // namespace

Application::Application() {
  redirectLogger("Application");
  AppRuntimeConfig runtime = ProductRuntimeConfig();
  runtime.name_thread = os::SetCurrentThreadName;
  AppRuntime::Initialize(runtime);
  secrets_ = std::make_unique<ProfileSecretsEngine>();
  // Registered before Bootstrap so a PIN-from-environment unlock reaches it too; main() applies the
  // secrets once the SessionStore exists, and the callback covers every later unlock.
  config_secrets_ = std::make_unique<ConfigSecretsStore>(*secrets_);
  secrets_->RegisterDekConsumer(config_secrets_.get());
  store_.SetConfigSecrets(config_secrets_.get());
  config_secrets_->SetOnReady([this] {
    if (!store_.IsInitialized()) {
      return;
    }
    if (auto applied = store_.ApplyConfigSecrets(); !applied) {
      log().warning << "config secrets not applied: " << applied.error().message;
    }
  });
  product_ = std::make_unique<ProductStack>();
  messaging_ = &product_->Hub();
  messaging_->BindSecrets(*secrets_);
  messaging_facade_ = std::make_unique<ConversationsFacade>(*messaging_);
  product_->BindFacade(*messaging_facade_);
  config_apply_ = std::make_unique<ConfigApplyBridge>();
  client_compat_ = std::make_unique<ClientCompatController>();
  badges_ = std::make_unique<BadgeAggregator>();
  input_ = std::make_unique<InputCoordinator>();
  flow_ = std::make_unique<FlowCoordinator>();
  shell_ = std::make_unique<ShellHost>();
  call_ = std::make_unique<CallController>();
  // PIN locked = the vault is not open (the PIN gate is up): a ring on the phone's call screen stays there meanwhile.
  call_->BindPinLocked([this]() { return !secrets_ || !secrets_->IsUnlocked(); });
  if constexpr (kDevToolsBuild) {
    // Android merge check for the mic rebuild: `adb shell setprop debug.pp.skip_route_reorder 1` makes the next
    // speaker tap change the route the old way (before the mic closes); set it back to 0 before arming it again.
    call_->SetDevRouteReorderSkip([armed = std::make_shared<bool>(true)]() {
      if (!os::DebugFlagSet("debug.pp.skip_route_reorder")) {
        *armed = true;
        return false;
      }
      return std::exchange(*armed, false);
    });
    // DEV/PLAN-call-ui-never-detached.md 6 (Android): `adb shell setprop debug.pp.fail_call_read 1` makes reading this
    // device's call from profile.db fail (the call screen must stay); `debug.pp.detach_call_ui 1` makes the screen drop
    // its call id after each refresh (the guard must hang the call up). Set back to 0 after.
    product_->Calls().Stack().SetDevFailCallRead([]() { return os::DebugFlagSet("debug.pp.fail_call_read"); });
    call_->SetDevDetachUi([]() { return os::DebugFlagSet("debug.pp.detach_call_ui"); });
  }
  call_ui_ = std::make_unique<CallUiBackend>(product_->Calls().Stack());
  settings_ = std::make_unique<SettingsController>();
  contacts_ = std::make_unique<ContactsController>();
  qr_scan_ = std::make_unique<QrScanController>(MediaDeviceArbiter::Default());
  avatar_crop_ = std::make_unique<AvatarCropController>();
  avatar_view_ = std::make_unique<AvatarViewController>();
  ContactsController::InstallInstance(*contacts_);
  contacts_shell_bridge_ = std::make_unique<ContactsShellBridge>();
  chat_shell_bridge_ = std::make_unique<ChatShellBridge>();
  people_picker_shell_bridge_ = std::make_unique<PeoplePickerShellBridge>();
  people_picker_ = std::make_unique<PeoplePickerController>();
  PeoplePickerController::InstallInstance(*people_picker_);
  emoji_picker_ = std::make_unique<EmojiPickerController>();
  EmojiPickerController::InstallInstance(*emoji_picker_);
  if constexpr (kDevToolsBuild) {
    ui_perf_ = std::make_unique<UiPerfTestController>();
  }
  chat_ = std::make_unique<ChatController>();
  unlock_gate_ = std::make_unique<ProfileUnlockGate>();
  pin_gate_ = std::make_unique<PinGateController>();
  onboarding_ = std::make_unique<OnboardingController>();
  OnboardingController::InstallInstance(*onboarding_);
  g_input_coordinator = input_.get();
}

Application::~Application() {
  PendingNotificationTap::Instance().SetRecordedHandler(nullptr);
  EmojiPickerController::ClearInstance();
  PeoplePickerController::ClearInstance();
  ContactsController::ClearInstance();
  Shutdown();
  messaging_facade_.reset();
  messaging_ = nullptr;
  product_.reset();
}

ConversationsHub& Application::Conversations() {
  return *messaging_;
}

ProfileSecretsEngine& Application::Secrets() {
  return *secrets_;
}

void Application::ShutdownMessaging() {
  if (!messaging_ || !messaging_->IsInitialized()) {
    if (secrets_ && secrets_->IsInitialized()) {
      StartupPhase phase("Shutdown::ProfileSecrets");
      secrets_->Shutdown();
    }
    return;
  }
  {
    StartupPhase phase("Shutdown::ConversationsHub");
    messaging_->Shutdown();
  }
  if (secrets_ && secrets_->IsInitialized()) {
    StartupPhase phase("Shutdown::ProfileSecrets");
    secrets_->Shutdown();
  }
}

Roe<void> Application::ResetActiveProfile() {
  const BootstrapResult& bootstrap = store_.Snapshot();
  const std::string profile_dir = bootstrap.profile_data_dir;
  const AppConfig config = bootstrap.config;

  if (profile_dir.empty()) {
    return AppError::Storage(Err::Storage::Unavailable, "Profile path unavailable");
  }

  log().info << "Resetting profile data at " << profile_dir;

  // #328: take the registration off the directory before the local wipe, so the name does not
  // linger in search until it expires. Best effort — a reset must still work offline.
  if (messaging_facade_ && messaging_facade_->IsInitialized() && messaging_facade_->IsMessagingReady()) {
    if (auto revoked = messaging_facade_->RevokeRegistration(); !revoked) {
      log().warning << "Registration revoke failed, continuing with the local reset: " << revoked.error().message;
    }
  }

  if (auto stopped = StopMessagingForReset(/*with_secrets=*/true); !stopped) {
    return stopped.error();
  }

  // The PIN grace window's keychain entry outlives the profile directory: deleted right before the wipe, so a reset
  // that stopped short above keeps the window. The face / fingerprint entry too (the DEK goes with the profile).
  ForgetGrace();
  if (secrets_) {
    secrets_->DisableBiometric();
  }

  std::error_code ec;
  std::filesystem::remove_all(profile_dir, ec);
  if (ec) {
    log().error << "remove_all(" << profile_dir << "): " << ec.message();
    return AppError::Storage(Err::Storage::Failed, "Failed to delete profile data: " + ec.message());
  }

  AppPaths::EnsureDirs(profile_dir);
  if (auto manifest = EnsureProfileManifest(profile_dir); !manifest) {
    return manifest.error();
  }

  if (auto secrets = secrets_->Initialize(profile_dir); !secrets) {
    return secrets.error();
  }
  AttachGrace();
  AttachBiometric();
  secrets_->RegisterDekConsumer(config_secrets_.get()); // Shutdown above dropped every consumer

  if (auto hub = messaging_->Initialize(HubSettingsFor(config, profile_dir), profile_dir); !hub) {
    return hub.error();
  }
  WireHubCallbacks();

  if (auto prefs = store_.ReloadProfilePrefs(); !prefs) {
    return prefs.error();
  }

  chat_->OnProfileDataReset();
  contacts_->Refresh();
  return {};
}

Roe<void> Application::ResetChatData() {
  const BootstrapResult& bootstrap = store_.Snapshot();
  const std::string profile_dir = bootstrap.profile_data_dir;
  const AppConfig config = bootstrap.config;

  if (profile_dir.empty()) {
    return AppError::Storage(Err::Storage::Unavailable, "Profile path unavailable");
  }

  const std::string chats_dir = SqliteThreadsRoot(profile_dir);
  log().info << "Resetting chat data at " << chats_dir;

  // Public sessions outlive a chat reset: their epoch is what lets each pair start its next one
  // instead of this device rewinding its numbering where the peer still holds the old messages (D014, #378).
  std::vector<PeerSessionRecord> kept_sessions;
  if (messaging_ && messaging_->IsInitialized()) {
    if (IPeerSessionStore* psk = messaging_->PeerSessions()) {
      if (auto listed = psk->List()) {
        for (PeerSessionRecord& record : *listed) {
          if (record.key.channel == CryptoChannel::E2ePublic && record.master_psk_b64) {
            kept_sessions.push_back(std::move(record));
          }
        }
      } else {
        log().warning << "chat reset: public sessions not kept: " << listed.error().message;
      }
    }
  }

  if (auto stopped = StopMessagingForReset(/*with_secrets=*/false); !stopped) {
    return stopped.error();
  }

  // profile.db (catalog, outbox, peer sessions, group rosters) and one thread.db per chat; the
  // account (identity.enc, vault.bin), contacts.json and the preferences live beside threads/.
  std::error_code ec;
  std::filesystem::remove_all(chats_dir, ec);
  if (ec) {
    log().error << "remove_all(" << chats_dir << "): " << ec.message();
    return AppError::Storage(Err::Storage::Failed, "Failed to delete chat data: " + ec.message());
  }

  if (auto hub = messaging_->Initialize(HubSettingsFor(config, profile_dir), profile_dir); !hub) {
    return hub.error();
  }
  WireHubCallbacks();

  chat_->OnProfileDataReset();
  contacts_->Refresh();
  // The vault is still open: bring messaging back now rather than at the next send — then put the kept
  // sessions back (the stores encrypt them under the DEK they get at unlock), each pair marked to restart.
  unlock_gate_->EnsureUnlocked([this, kept = std::move(kept_sessions)](const bool unlocked) mutable {
    IPeerSessionStore* psk = messaging_ ? messaging_->PeerSessions() : nullptr;
    if (!unlocked || !psk) {
      return;
    }
    for (PeerSessionRecord& record : kept) {
      record.restart_pending = true;
      if (auto saved = psk->Save(record); !saved) {
        log().warning << "chat reset: a public session not kept: " << saved.error().message;
      }
    }
  });
  return {};
}

void Application::OfferChatStoreRecovery(const ChatStoreRecoveryPrompt& prompt) {
  if (chat_store_recovery_up_) {
    return; // already asking
  }
  chat_store_recovery_up_ = true;
  log().warning << "Chat store needs recovery: " << prompt.title_key;
  ShowChatStoreRecovery(
      prompt,
      [this]() {
        chat_store_recovery_up_ = false;
        // Reset chats as Me → Storage does; start-up then goes on (the hub comes back with a fresh chat store).
        if (auto reset = ResetChatData(); !reset) {
          UserFeedback::Fail(UserFeedback::FailFrom(reset.error()));
          return;
        }
        UserFeedback::Ok(Tr("settings.storage.chats_reset"));
      },
      // Declined: the next unlock attempt (any send or open of a chat) asks again.
      [this]() { chat_store_recovery_up_ = false; });
}

Roe<void> Application::StopMessagingForReset(const bool with_secrets) {
  // AI turns run on threads of their own, which the runtime's quiesce does not wait for, and read the thread store.
  if (agent_sessions_ && !agent_sessions_->StopTurnsAndWait(kProfileResetQuiesceBudget)) {
    return AppError::Storage(Err::Storage::Unavailable, "Background work is still running — try again");
  }
  qr_scan_->Close();
  avatar_crop_->Close();
  avatar_view_->Close();
  if (messaging_) {
    messaging_->RequestShutdown();
  }
  // Same teardown quiesce as quit, but the process lives on: reopen before re-initializing.
  if (!AppRuntime::QuiesceForTeardown(kProfileResetQuiesceBudget)) {
    AppRuntime::ReopenAfterTeardown();
    if (messaging_) {
      messaging_->CancelShutdownRequest();
    }
    return AppError::Storage(Err::Storage::Unavailable, "Background work is still running — try again");
  }
  if (with_secrets) {
    ShutdownMessaging();
  } else if (messaging_ && messaging_->IsInitialized()) {
    StartupPhase phase("Shutdown::ConversationsHub");
    messaging_->Shutdown();
  }
  // The data is gone (both variants): the new inbox starts empty and its first Refresh() sends the true value.
  // Not in ShutdownMessaging: a normal quit keeps the icon number while the messages are still unread.
  if (badges_) {
    badges_->ClearAppBadge();
  }
  AppRuntime::ReopenAfterTeardown();
  return {};
}

int Application::GraceMinutes() const {
  // merged from kenneth/pin-grace-settings: Me → Security's choice (0 / 5 / 15 / 60 / -1 = always, default 15).
  return store_.IsInitialized() ? store_.Snapshot().profile_prefs.unlock_grace_minutes : 0;
}

void Application::AttachGrace() {
  if (!secrets_ || !secrets_->IsInitialized()) {
    return;
  }
  secrets_->AttachGrace(
      std::make_unique<GraceUnlock>(OsSecureBlobStore(), secrets_->ProfileDataDir(), secrets_->ProfileId()),
      &os::GraceClockNow);
}

void Application::AttachBiometric() {
  if (!secrets_ || !secrets_->IsInitialized()) {
    return;
  }
  secrets_->AttachBiometric(
      std::make_unique<BiometricUnlock>(OsSecureBlobStore(), secrets_->ProfileDataDir(), secrets_->ProfileId()));
}

std::string Application::BiometricKind() const {
  if (!secrets_ || !secrets_->IsInitialized() || !secrets_->BiometricSupported()) {
    return {};
  }
  switch (os::BiometryKind()) {
    case os::Biometry::Face:
      return "face";
    case os::Biometry::Touch:
      return "touch";
    case os::Biometry::Fingerprint:
      return "fingerprint";
    case os::Biometry::None:
      break;
  }
  return {};
}

void Application::TouchGrace() {
  if (!secrets_ || !secrets_->IsInitialized() || !secrets_->IsUnlocked() || !secrets_->GraceSupported()) {
    return; // a locked vault's entry is the gate's to read (an expired one must not be renewed)
  }
  const int minutes = GraceMinutes();
  if (!GraceIsOn(minutes) || GraceIsAlways(minutes)) { // off, or "until restart": nothing to touch
    return; // off, or "Always" (kUnlockGraceAlways): that entry never runs out, nothing to extend
  }
  if (auto touched = secrets_->TouchGrace(minutes); !touched) {
    log().warning << "Grace window not extended: " << touched.error().message;
    DiagLine("unlock.grace").Add("result", "write_failed").Fail("error", touched.error()).Emit();
  }
}

void Application::StartGraceTimer() {
  if (grace_timer_ != 0 || !secrets_ || !secrets_->GraceSupported() || !GraceIsOn(GraceMinutes()) ||
      GraceIsAlways(GraceMinutes())) {
    return; // the window's length is read at each tick (TouchGrace): 0 extends nothing, "Always" needs no ticks
  }
  // A kill without a background event may cost up to one interval of the window, so the interval stays at most half
  // the window chosen (5 → 2 minutes) and at most 5 minutes (15 / 60): no keychain write every minute for nothing.
  const int every_minutes = std::clamp(GraceMinutes() / 2, 1, 5);
  grace_timer_ = AppRuntime::ScheduleCoordinatorRepeating(std::chrono::minutes(every_minutes), [this]() {
    AppRuntime::PostUI([this]() {
      if (grace_timer_ != 0 && AppLifecycle::IsForeground()) {
        TouchGrace();
      }
    });
  });
}

void Application::StopGraceTimer() {
  if (grace_timer_ == 0) {
    return;
  }
  AppRuntime::CancelCoordinatorTimer(grace_timer_);
  grace_timer_ = 0;
}

void Application::ForgetGrace() {
  StopGraceTimer();
  if (secrets_) {
    secrets_->ForgetGrace();
  }
}

std::string Application::AssetsPath(const std::string& relative) {
  return IAssetLocator::Instance().Resolve(relative);
}

bool Application::InitializeUiHost(const char* window_title, int window_width, int window_height,
                                   const BootstrapResult& bootstrap, ui::Context*& context) {
  AppRuntime::InitializeUI();
  // A blocking HTTP request on the UI thread holds every frame until it is answered: leave a net.ui_block line.
  HttpClient::SetUiThreadProbe([] { return AppRuntime::CurrentlyOnUI(); });

  if (![&] {
        StartupPhase phase("Backend::Initialize");
        return Backend::Initialize(window_title, window_width, window_height, true, !Platform::IsMobile());
      }()) {
    log().error << "Backend::Initialize failed (SDL/OpenGL window could not be created)";
    return false;
  }
  // PostUI must imply RequestForceFrame: skip idle wait and Present soon (THREADING.md
  // UI delivery). WakeEventLoop alone is not enough on platforms where WaitEventTimeout lied.
  AppRuntime::SetUIWakeCallback([]() { Backend::RequestForceFrame(); });

#if UI_SDL_VERSION_MAJOR >= 3
  if (!Platform::IsMobile()) {
    if (auto* window = Backend::GetWindow()) {
      if (!SetWindowIconFromAsset(window, kAppIconAsset)) {
        log().warning << "Failed to load window icon from " << kAppIconAsset;
      }
    }
  }
#endif

  ui::SetSystemInterface(Backend::GetSystemInterface());
  ui::SetRenderInterface(Backend::GetRenderInterface());

  if (ui::FileInterface* packaged_files = PlatformHooks::PackagedFileInterface()) {
    ui::SetFileInterface(packaged_files);
  }

  harfbuzz_font_engine_ = std::make_unique<FontEngineInterfaceHarfBuzz>();
  harfbuzz_font_engine_->RegisterLanguage("en", "Latn", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("zh-Hans", "Hans", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("zh-Hant", "Hant", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("ja", "Jpan", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("ko", "Kore", TextFlowDirection::LeftToRight);
  ui::SetFontEngineInterface(harfbuzz_font_engine_.get());

  if (![&] {
        StartupPhase phase("ui::Initialise");
        return ui::Initialise();
      }()) {
    log().error << "ui::Initialise failed";
    Backend::Shutdown();
    return false;
  }
  RegisterCallVideoTileElement();
  RegisterScanPreviewElement();
  RegisterAvatarCropElement();

#ifdef PPBROWSER_ENABLE_DEBUGGER
  ui::Debugger::Initialise(ui::CreateContext("debugger", ui::Vector2i(0, 0)));
#endif

  const std::string theme_path = AssetsPath(bootstrap.profile_prefs.theme);
  Theme::LoadBase(theme_path);
  {
    StartupPhase phase("LoadFontFace:LatoLatin");
    ui::LoadFontFace(AssetsPath("fonts/LatoLatin-Regular.ttf"));
  }
  {
    // Family "Inter" in four weights (each file's own weight); RCSS opts in with font-family: Inter.
    StartupPhase phase("LoadFontFace:Inter");
    for (const char* file :
         {"fonts/Inter-Regular.ttf", "fonts/Inter-Medium.ttf", "fonts/Inter-SemiBold.ttf", "fonts/Inter-Bold.ttf"}) {
      ui::LoadFontFace(AssetsPath(file));
    }
  }

  {
    StartupPhase phase("Localization::LoadFromAssets");
    // The platform's own network tip for the key capabilities use (common/Localized.h).
    LocalizationService::Instance().SetKeyAlias(kPlatformNetworkHintKey,
                                                std::string(PlatformUserHints::P2pNetworkHintKey()));
    if (auto loaded = LocalizationService::Instance().LoadFromAssets(IPathProvider::Instance().BundleAssetsDir());
        !loaded) {
      log().warning << "Localization catalogs failed to load: " << loaded.error().message;
    }
    LocalizationService::Instance().SetPreferredLanguage(bootstrap.profile_prefs.language);
  }

  context = ui::CreateContext("main", ui::Vector2i(window_width, window_height));
  if (!context) {
    log().error << "ui::CreateContext failed";
    ui::Shutdown();
    Backend::Shutdown();
    return false;
  }

  context->SetTextLoupeRenderCallback(
      [context](ui::TextLoupePhase phase, const ui::TextLoupeState& state, ui::RenderManager&) {
        TextLoupeRenderer::Render(phase, state, static_cast<RenderInterface_GL3&>(*Backend::GetRenderInterface()),
                                  context->GetDensityIndependentPixelRatio());
      });

  Backend::SyncContext(context);

  const AppearanceMode appearance = Theme::ParseAppearance(bootstrap.profile_prefs.appearance);
  Theme::ApplyAppearance(context, appearance);

  SetAppEventHooks(AppEventHooks{
      .on_sync_system_theme = [](ui::Context* ctx) { Theme::SyncSystemTheme(ctx); },
      .on_context_pointer = [](ui::Context* ctx, int x,
                               int y) { return ContextMenuHost::Instance().OnContextPointer(ctx, x, y); },
  });
  SdlAppEvents::Install();
  InstallStayConnectedService(); // P011: Android's foreground service follows the setting

  ContextMenuHost::Instance().Install(context);
  return true;
}

SettingsToolPorts Application::WireSettings(ui::Context* context) {
  ConversationsFacade& facade = *messaging_facade_;
  SettingsCommands settings_commands;
  settings_commands.load_profile_identity = [&facade]() { return facade.LoadProfileIdentityView(); };
  settings_commands.save_profile_nickname = [&facade](const std::string& nickname) {
    return facade.SaveProfileNickname(nickname);
  };
  settings_commands.pick_profile_icon_image = [](std::function<void(std::vector<std::string> paths)> on_picked) {
    // iOS: a large photo takes seconds to copy out of the library; the pill says so until the crop page is opened
    // (on_picked opens it), and is taken down first so it never sits over that page. Cancelled: no pill, nothing to take down.
    auto preparing_shown = std::make_shared<bool>(false);
    ShowOpenImageFileDialog(
        Backend::GetWindow(),
        [on_picked = std::move(on_picked), preparing_shown](std::vector<std::string> paths) {
          AppRuntime::PostUI([on_picked = std::move(on_picked), preparing_shown, paths = std::move(paths)]() mutable {
            if (*preparing_shown) {
              UserFeedback::DismissToast();
            }
            on_picked(std::move(paths));
          });
        },
        /*include_heic=*/false,
        [preparing_shown](const std::string&) {
          AppRuntime::PostUI([preparing_shown]() {
            *preparing_shown = true;
            UserFeedback::Note(Tr("settings.profile.photo_preparing"));
          });
        });
  };
  settings_commands.open_avatar_crop = [this](std::shared_ptr<PickedFileLease> file, AvatarCropCommit commit) {
    avatar_crop_->Open(std::move(file), std::move(commit));
  };
  settings_commands.open_avatar_viewer = [this](const std::string& image_src) { avatar_view_->Open(image_src); };
  settings_commands.upload_profile_icon_file = [&facade](const std::string& path,
                                                         std::optional<ProfileIconCrop> crop) -> Roe<void> {
    // The image is the app's to decode and size; the hub uploads what comes out.
    auto encoded = PrepareProfileIconFromFile(path, 512u * 1024u, 512, std::nullopt, crop);
    if (!encoded) {
      return encoded.error();
    }
    PreparedProfileIcon prepared;
    prepared.bytes = std::move(encoded->bytes);
    prepared.content_type = std::move(encoded->content_type);
    prepared.kind = std::move(encoded->kind);
    prepared.file_extension = std::move(encoded->file_extension);
    return facade.UploadProfileIcon(prepared);
  };
  settings_commands.clear_profile_icon = [&facade]() { return facade.ClearProfileIcon(); };
  settings_commands.plan_relay_quota_recovery = [&facade](const Error& quota_error) {
    return facade.PlanRelayQuotaRecovery(quota_error);
  };
  settings_commands.delete_relay_file = [&facade](const std::string& blob_id) {
    return facade.DeleteRelayFileById(blob_id);
  };
  // Me → Storage works on the hub's content store: open from messaging's Initialize to its teardown.
  const auto content_store = [this]() -> std::shared_ptr<CasStore> {
    return messaging_ != nullptr ? messaging_->Content() : nullptr;
  };
  settings_commands.list_cas_library = [content_store](std::string filter) -> Roe<std::vector<CasLibraryItemView>> {
    const auto content = content_store();
    if (!content) {
      return Error("Messaging is not ready");
    }
    return ListCasLibraryForSettings(*content, std::move(filter));
  };
  settings_commands.share_cas_publicly = [this, content_store](const std::string& private_content_id_hex) -> Roe<void> {
    const auto content = content_store();
    if (!content) {
      return Error("Messaging is not ready");
    }
    if (!secrets_ || !secrets_->IsUnlocked()) {
      return Error("Unlock your profile to share publicly");
    }
    DataKeyVault* vault = secrets_->Vault();
    if (vault == nullptr) {
      return Error("Vault unavailable");
    }
    auto dek = vault->Dek();
    if (!dek) {
      return dek.error();
    }
    return ShareCasPubliclyForSettings(*content, *dek, private_content_id_hex);
  };
  settings_commands.unpublish_cas = [content_store](const std::string& public_content_id_hex) -> Roe<void> {
    const auto content = content_store();
    if (!content) {
      return Error("Messaging is not ready");
    }
    return UnpublishCasForSettings(*content, public_content_id_hex);
  };

  settings_commands.fetch_cas_public_tip = [this](const std::string& tip, const std::string& peer_relay_user_id,
                                                  std::function<void(Roe<void>)> on_done) {
    if (!secrets_ || !secrets_->IsInitialized()) {
      on_done(Error("Profile secrets are not ready"));
      return;
    }
    if (!messaging_ || !messaging_->IsInitialized()) {
      on_done(Error("Messaging is not ready"));
      return;
    }
    auto* blob = messaging_->MeshMessaging().PeerBlobClient();
    if (blob == nullptr) {
      on_done(Error("Peer blob client is not available"));
      return;
    }
    auto local = messaging_->Identity().Get();
    if (!local) {
      on_done(local.error());
      return;
    }
    // The fetch rides the mesh (never parks UI); its answer comes back to UI.
    FetchCasPublicTipForSettingsAsync(messaging_->Content(), *blob, local->relay_user_id, tip, peer_relay_user_id,
                                      [on_done = std::move(on_done)](Roe<void> fetched) {
                                        AppRuntime::PostUI([on_done, fetched]() { on_done(fetched); });
                                      });
  };
  settings_commands.register_identity = [this, &facade](const RegisterIdentityArgs& args) {
    auto result = facade.RegisterIdentity(args.nickname);
    if (result) {
      chat_->ReloadAgentConfig();
    }
    return result;
  };
  settings_commands.rotate_brief_llm_key = [this, &facade]() {
    auto result = facade.RotateBriefLlmKey(BriefLlmServiceBaseUrl(store_.Snapshot().config));
    if (result) {
      chat_->ReloadAgentConfig();
    }
    return result;
  };
  settings_commands.clear_undelivered_older_than = [&facade](int days) -> Roe<void> {
    if (!facade.IsInitialized() || !facade.IsMessagingReady()) {
      return Error("Messaging is not ready");
    }
    if (auto cleared = facade.ClearUndeliveredOlderThan(days); !cleared) {
      return cleared.error();
    }
    return {};
  };
  settings_commands.run_reachability_probe = [&facade](bool try_upnp) { facade.RunReachabilityProbe(try_upnp); };
  settings_commands.try_upnp_port_mapping = [&facade]() { facade.TryUpnpPortMapping(); };
  settings_commands.reset_active_profile = [this]() { return ResetActiveProfile(); };
  settings_commands.set_backend_switch_pending = [&facade](const bool pending) {
    facade.SetBackendSwitchPending(pending);
  };
  settings_commands.reset_chat_data = [this]() { return ResetChatData(); };
  settings_commands.language_display_label = [](const std::string& pref) {
    return LocalizationService::Instance().LanguageDisplayLabel(pref);
  };
  settings_commands.available_locales = []() { return LocalizationService::Instance().AvailableLocales(); };
  settings_commands.apply_appearance = [](const std::string& appearance_pref) {
    // Settings tools run on the worker pool; RmlUi theme activation is UI-thread only.
    AppRuntime::PostUI([appearance_pref]() {
      if (auto* ctx = ui::GetContext("main")) {
        Theme::ApplyAppearance(ctx, Theme::ParseAppearance(appearance_pref));
      }
    });
  };
  settings_commands.session_store = [this]() -> SessionStore& { return store_; };
  settings_commands.reload_from_disk = [this]() { return store_.ReloadFromDisk(); };
  settings_commands.messaging_ready = [&facade]() { return facade.IsInitialized() && facade.IsMessagingReady(); };
  settings_commands.mesh_ready = [&facade]() { return facade.IsInitialized() && facade.IsMeshReady(); };
  settings_commands.last_mesh_error = [&facade]() -> std::string {
    if (!facade.IsInitialized()) {
      return {};
    }
    return facade.LastMeshError();
  };
  settings_commands.amp_listen_multiaddr = [&facade]() -> std::string {
    if (!facade.IsInitialized()) {
      return {};
    }
    if (const mesh::MeshHost* mesh = facade.Hub().Mesh()) {
      const auto advertised = mesh->AdvertisedListenMultiaddrs();
      if (!advertised.empty()) {
        return advertised.front();
      }
      return mesh->AmpListenMultiaddr();
    }
    return {};
  };
  settings_commands.load_reachability = [&facade]() {
    SettingsReachabilityView view;
    if (!facade.IsInitialized()) {
      return view;
    }
    const mesh::ReachabilitySnapshot snap = facade.Reachability();
    switch (snap.status) {
      case mesh::ReachabilityStatus::Checking:
        view.status = SettingsReachabilityView::Status::Checking;
        break;
      case mesh::ReachabilityStatus::Reachable:
        view.status = SettingsReachabilityView::Status::Reachable;
        break;
      case mesh::ReachabilityStatus::OutboundOnly:
        view.status = SettingsReachabilityView::Status::OutboundOnly;
        break;
      case mesh::ReachabilityStatus::Blocked:
        view.status = SettingsReachabilityView::Status::Blocked;
        break;
      case mesh::ReachabilityStatus::Unknown:
      default:
        view.status = SettingsReachabilityView::Status::Unknown;
        break;
    }
    view.has_global_ipv6 = snap.signals.has_global_ipv6;
    view.dial_back_ok = snap.signals.dial_back_ok;
    view.upnp_mapped = snap.signals.upnp_mapped;
    view.help_kind = mesh::ReachabilityHelpKey(snap.status);
    return view;
  };
  settings_commands.refresh_nav_badges = [this]() {
    if (badges_) {
      badges_->Refresh();
    }
  };
  settings_commands.load_pin_protection = [this]() {
    PinProtectionView view;
    ProfileSecretsEngine& secrets = *secrets_;
    view.ready = secrets.IsInitialized() && secrets.HasVault();
    view.unlocked = view.ready && secrets.IsUnlocked();
    return view;
  };
  settings_commands.change_pin = [this](const SecretText& current_pin, const SecretText& new_pin) -> Roe<void> {
    if (secrets_->Vault() == nullptr) {
      return AppError::Pin(Err::Pin::VaultUnavailable, "Vault unavailable");
    }
    // The engine's ChangePin forgets the PIN grace window on success: the next cold start asks for the new PIN.
    if (auto changed = secrets_->ChangePin(current_pin.Reveal(), new_pin.Reveal()); !changed) {
      return changed.error();
    }
    StopGraceTimer();
    return {};
  };
  settings_commands.export_link_device = [&facade]() -> Roe<std::string> { return facade.ExportLinkDevice(); };
  // merged from kenneth/pin-grace-settings: Me → Security's "PIN again after" row (PIN grace window, at-rest A013).
  settings_commands.grace_supported = [this]() { return secrets_->GraceSupported(); };
  settings_commands.set_grace_minutes = [this](const int minutes) {
    if (minutes == 0) {
      ForgetGrace(); // both pieces and the foreground timer
      return;
    }
    // The new length at once (a 15-minute window becomes 5), and an entry when none existed yet ("Until restart"
    // chosen after "Immediately"), written from the unlocked vault.
    if (secrets_->IsUnlocked()) {
      if (auto touched = secrets_->ApplyGraceWindow(minutes); !touched) {
        log().warning << "Grace window not changed: " << touched.error().message;
        DiagLine("unlock.grace").Add("result", "write_failed").Fail("error", touched.error()).Emit();
      }
    }
    if (AppLifecycle::IsForeground()) {
      StopGraceTimer();  // its interval follows the window's length
      StartGraceTimer(); // kept running, or back after "Immediately"; not started for "Always"
    }
  };
  // Face / fingerprint unlock (at-rest A014): Me → Security's switch. Enabling runs Argon2 (the caller is a worker).
  settings_commands.biometric_kind = [this]() { return BiometricKind(); };
  settings_commands.biometric_enabled = [this]() { return secrets_ && secrets_->BiometricEnabled(); };
  settings_commands.enable_biometric = [this](const SecretText& pin) -> Roe<void> {
    if (!secrets_) {
      return AppError::Pin(Err::Pin::VaultUnavailable, "Profile secrets unavailable");
    }
    return secrets_->EnableBiometric(pin.Reveal());
  };
  settings_commands.disable_biometric = [this]() {
    if (secrets_) {
      secrets_->DisableBiometric();
    }
  };
  settings_commands.recovery_phrase = [&facade](const SecretText& pin) { return facade.RecoveryPhrase(pin); };
  settings_commands.verify_pin = [this](const SecretText& pin) { return secrets_->VerifyPin(pin.Reveal()); };
  settings_commands.has_recovery_phrase = [&facade]() { return facade.HasRecoveryPhrase(); };
  settings_commands.export_account_copy = [&facade]() { return facade.ExportAccountCopy(); };
  settings_commands.own_devices = [&facade]() -> Roe<OwnDevicesView> {
    auto list = facade.OwnDevices();
    if (!list) {
      return list.error();
    }
    std::vector<std::string> peer_ids;
    peer_ids.reserve(list->devices.size());
    for (const AccountCardDevice& device : list->devices) {
      peer_ids.push_back(device.peer_id);
    }
    return OwnDevicesViewFor(peer_ids, list->self_peer_id, list->signed_out);
  };
  settings_commands.remove_own_device = [&facade](const std::string& peer_id) {
    return facade.RemoveOwnDevice(peer_id);
  };
  settings_commands.make_main_device = [&facade](const std::string& peer_id) { return facade.MakeMainDevice(peer_id); };
  settings_commands.load_support_discovery = [this]() -> std::optional<ClientCompatSupport> {
    return support_discovery_;
  };
  settings_commands.blocked_count = [this]() { return contacts_ ? contacts_->BlockedCount() : 0; };
  settings_commands.open_blocked_people = [this]() {
    if (contacts_) {
      contacts_->OpenBlockedPeople();
    }
  };
  settings_commands.open_support_chat = [this, &facade](const std::string& prefilled_text) -> Roe<void> {
    if (!support_discovery_ || !support_discovery_->enabled || support_discovery_->account_id.empty()) {
      return Error(Tr("support.unavailable"));
    }
    if (!facade.IsInitialized()) {
      return Error("Messaging is not ready");
    }

    const std::string account_id = support_discovery_->account_id;
    // The contact is named in the user's language ("PP 客服" / "PP Support"), not by the server's English display
    // name (Kenneth 2026-11-09); the discovery name is only the fallback when the locale has no title.
    std::string display_name = Tr("support.entry.title");
    if (display_name.empty()) {
      display_name = support_discovery_->display_name;
    }

    auto found = facade.FindContactByIdentity(account_id, mesh::ContactIdKind::Account);
    if (!found) {
      return found.error();
    }

    std::string contact_id;
    if (found->has_value()) {
      Contact contact = **found;
      if (ApplySupportTitle(contact, display_name)) {
        auto saved = Conversations().Contacts().Upsert(contact);
        if (!saved) {
          return saved.error();
        }
        contact_id = saved->id;
      } else {
        contact_id = (*found)->id;
      }
    } else {
      Contact contact = MakeSupportContact(util::GenerateUuid(), account_id, display_name);
      auto saved = Conversations().Contacts().Upsert(contact);
      if (!saved) {
        return saved.error();
      }
      contact_id = saved->id;
    }

    auto thread = facade.FindOrCreateDirectThread(contact_id, ThreadChannel::E2ePublic);
    if (!thread) {
      return thread.error();
    }

    const ShellNavigationPorts nav = MakeShellNavigationPorts(*shell_);
    if (nav.select_nav_tab) {
      nav.select_nav_tab(NavTab::Sessions);
    }
    if (nav.set_primary_pane) {
      nav.set_primary_pane("chat");
    }
    if (chat_) {
      chat_->FinalizeThreadDisplay();
      chat_->PrefillDraft(prefilled_text);
    }
    return {};
  };
  settings_commands.ask_support =
      [this, open = settings_commands.open_support_chat](const std::string& question) -> Roe<void> {
    if (auto opened = open({}); !opened) {
      return opened;
    }
    if (chat_) {
      chat_->SendTypedText(question);
    }
    return {};
  };
  settings_commands.ask_ai_once = [this](const std::string& question) { return chat_ && chat_->AskAiOnce(question); };
  if (chat_) {
    chat_->BindAskSupport(settings_commands.ask_support); // the ask-AI panel's "Ask PP Support"
  }
  settings_commands.send_diagnostics = [this, &facade](std::function<void(DiagnosticsUploadResult)> on_done) {
    DiagnosticsUploadDeps deps;
    deps.data_dir = store_.Snapshot().data_dir;
    // The bundle's self-check refuses to send when any of these appears in it (a line that got past the shareable rule).
    if (auto identity = facade.GetIdentity()) {
      deps.known_identifiers = {identity->account_id, identity->peer_id, identity->relay_user_id};
    }
    // The self-check strips a leading scheme itself; the bare ids are passed too so either form is caught.
    auto add_bare = [&deps](const std::string& id) {
      for (const char* scheme : {"account:", "relay:", "peer:", "call:"}) {
        const std::string prefix = scheme;
        if (id.compare(0, prefix.size(), prefix) == 0 && id.size() > prefix.size()) {
          deps.known_identifiers.push_back(id.substr(prefix.size()));
          return;
        }
      }
    };
    for (const std::string& id : std::vector<std::string>(deps.known_identifiers)) {
      add_bare(id);
    }
    if (auto contacts = Conversations().Contacts().List()) {
      for (const Contact& contact : *contacts) {
        for (const auto& id : contact.ids) {
          deps.known_identifiers.push_back(id.value);
          add_bare(id.value);
        }
      }
    }
    // The AI stream's key: registered, else guest, else mint a guest key now (same helpers as the chat).
    deps.bearer = [&facade]() -> Roe<std::string> {
      auto identity = facade.GetIdentity();
      if (!identity) {
        return Error("Messaging is not ready");
      }
      std::string key =
          ResolveBriefLlmApiKey(identity->brief_llm_api_key.Reveal(), identity->brief_llm_guest_api_key.Reveal());
      if (!key.empty()) {
        return key;
      }
      if (identity->account_id.empty()) {
        return Error("Messaging is not ready"); // no account to mint for: nothing minted, nothing written
      }
      auto minted = MintBriefGuestLlmKey(BriefLlmBaseUrl());
      if (!minted) {
        return minted.error();
      }
      // Off the UI thread: one modify of the guest key field only, into the account it was minted for, keeping a key
      // stored meanwhile (that one is the Bearer then).
      return StoreBriefGuestKeyIfNone(MakeBriefLlmKeyStore(facade), minted->llm_api_key, identity->account_id);
    };
    SendDiagnostics(std::move(deps), std::move(on_done));
  };
  settings_commands.last_diagnostics_id = [this]() -> std::string {
    const auto last = ReadLastUpload(store_.Snapshot().data_dir);
    return last ? last->upload_id : std::string();
  };
  settings_commands.storage = MakeStorageManagerPorts(facade, [this]() { return store_.Snapshot().profile_data_dir; });
  // Copy ports before BindCommands moves the command bag.
  SettingsToolPorts settings_tool_ports = SettingsToolPortsFromCommands(settings_commands);
  settings_tool_ports.open_support_chat = [open = settings_commands.open_support_chat]() {
    return RunOnUiAndWait([open]() { return open({}); });
  };
  settings_->BindCommands(std::move(settings_commands));
  // SettingsController is constructed before locale catalogs load; rebuild Tr()-backed
  // preference row titles/subtitles (and other localized labels) now that catalogs exist.
  settings_->RefreshLocalizedChrome();
  return settings_tool_ports;
}

void Application::LogCallTapTiming() {
  const uint64_t seq = PendingNotificationTap::Instance().CallTapSeq();
  if (seq == logged_call_tap_seq_.exchange(seq)) {
    return; // not a call tap
  }
  const int64_t now_ms = MsSinceStartup();
  const int64_t first_ms = first_present_ms_.load();
  if (first_ms < 0) {
    log().info << "Call tap recorded before the first frame (" << now_ms << " ms after launch)";
  } else {
    log().info << "Call tap recorded " << (now_ms - first_ms) << " ms after the first frame";
  }
}

void Application::WireShellPresenters(const SettingsToolPorts& settings_tool_ports) {
  ConversationsHub& messaging = Conversations();
  ConversationsFacade& facade = *messaging_facade_;
  ShellHost& shell = *shell_;
  const ShellNavigationPorts shell_navigation = MakeShellNavigationPorts(shell);
  const ShellFeedbackPorts shared_feedback = BindSharedShellFeedback(shell);
  settings_->BindShellFeedback(shared_feedback);
  settings_->BindShellNavigation(shell_navigation);

  chat_->BindSessionStore(store_);
  contacts_->BindSessionStore(&store_);

  contacts_->BindShellNavigation(shell_navigation);
  contacts_->BindShellFeedback(shared_feedback);
  qr_scan_->BindShellNavigation(shell_navigation);
  avatar_crop_->BindShellNavigation(shell_navigation);
  avatar_view_->BindShellNavigation(shell_navigation);
  qr_scan_->BindShowMyQr([this]() { contacts_->OpenMyQr(); });
  contacts_->BindScanPorts(ScanPorts{
      .scan_qr = [this](QrScanResultCallback done) { qr_scan_->Open(std::move(done)); },
  });
  contacts_->BindAccountPorts(ContactsAccountPorts{
      .load_identity = [&facade]() { return facade.LoadProfileIdentityView(); },
  });
  shell.BindScanAction([this]() { contacts_->OnScanQr(); });
  contacts_shell_bridge_->BindApply(MakeShellChromeApplyPorts(shell, "contacts_chrome"));
  contacts_->BindSurfaceNotify(ContactsSurfaceNotifyPorts{
      .push_surface = [this](const ContactsSurfaceSnapshot& snap) { contacts_shell_bridge_->OnSurface(snap); },
  });

  chat_->BindShellNavigation(shell_navigation);
  chat_->BindShellFeedback(shared_feedback);
  settings_->BindOpenHomeEdit([this]() { chat_->OpenHomeEdit(); });
  chat_->BindOpenSettingsSection([this](const std::string& section_id) { settings_->OpenSection(section_id); });
  // B216: the chats list's reconnect row and alert register through Me → Profile's action; a registration the user
  // asks for never overlaps the automatic renewal (it waits for the one in flight).
  chat_->BindReconnectAccount(
      [this](std::function<void(bool ok)> done) { settings_->ReconnectAccount(std::move(done)); });
  settings_->BindRunWhenRegistrationIdle(
      [this](std::function<void()> run) { chat_->RunWhenRegistrationIdle(std::move(run)); });
  chat_shell_bridge_->BindApply(MakeShellChromeApplyPorts(shell, "chat_chrome"));
  chat_->BindSurfaceNotify(ChatSurfaceNotifyPorts{
      .push_surface = [this](const ChatSurfaceSnapshot& snap) { chat_shell_bridge_->OnSurface(snap); },
  });
  chat_->BindShellSetup(MakeShellSetupPorts(shell));
  chat_->BindConversationsFacade(messaging_facade_.get());
  chat_->BindRegisterMessagingTools([this, settings_tool_ports](ToolRegistry& tools) {
    RegisterMessagingTools(tools, *messaging_facade_);
    RegisterSettingsTools(tools, settings_tool_ports);
  });
  MessagingUiPorts messaging_ui;
  messaging_ui.snapshot = [&messaging]() { return ProjectMessagingView(messaging); };
  chat_->BindMessagingUi(std::move(messaging_ui));

  contacts_->BindContactsPorts(MakeMessagingContactsPorts(messaging));
  // A failed message's "update their card" is the contact page's refresh (the same hub call).
  chat_->BindRefreshContact(MakeMessagingContactsPorts(messaging).refresh_contact);
  call_->BindCallPorts(MakeCallFunctionalPorts(*call_ui_, messaging, store_.IsInitialized() ? &store_ : nullptr));
  call_->BindShellCallChrome(MakeShellCallChromePorts(shell));
  call_->BindPeerLinkRefresh([this]() {
    if (chat_) {
      chat_->UpdatePeerLinkChrome();
    }
  });
  call_->BindOpenThread([this](const std::string& thread_id) {
    if (chat_) {
      chat_->OnSelectThread(thread_id);
    }
  });
  pin_gate_->BindShellPinGate(MakeShellPinGatePorts(shell));
  {
    // DEV/PLAN-pin-gate-redesign.md B: the number pad on phones only; desktops keep the field and the keyboard.
    PinGateKeypadPorts keypad;
    keypad.keypad_available = []() { return Platform::IsMobile(); };
    keypad.pin_digits = [this]() -> std::optional<int> {
      return store_.IsInitialized() ? store_.Snapshot().profile_prefs.pin_digits : std::nullopt;
    };
    // Face / fingerprint unlock on (A014): the unlock door's key.
    keypad.biometric_kind = [this]() -> std::string {
      return secrets_ && secrets_->BiometricEnabled() ? BiometricKind() : std::string();
    };
    pin_gate_->BindKeypad(std::move(keypad));
  }
  // A push's call came or went (Android's call notification, P014): the PIN screen words itself by it, and the call
  // screen looks again for the ring it announced (an Answer made on it before the ring was known applies at once).
  ICallSystemUi::Instance().SetPendingChangedHandler([this]() {
    pin_gate_->RefreshCallHint();
    if (call_) {
      call_->OnCallWake();
    }
  });
  // A tap arrives off the UI thread; the PIN screen words itself by it.
  PendingNotificationTap::Instance().SetRecordedHandler([this]() {
    LogCallTapTiming();
    AppRuntime::PostUI([this]() { pin_gate_->RefreshCallHint(); });
  });
  debug_call_tap_delay_ms_ = DebugCallTapDelayMs();
  if (debug_call_tap_delay_ms_ == 0) {
    RecordDebugCallTap(); // before the first frame
  }
  flow_->BindShellNavigation(shell_navigation);
  people_picker_->BindContactsPorts(MakeMessagingContactsPorts(messaging));
  MessagingPeoplePickerPorts picker_ports = product_->PeoplePickerPorts();
  picker_ports.invite_members_async = [facade = messaging_facade_.get()](
                                          const std::string& group_id, std::vector<std::string> contact_ids,
                                          std::function<void(GroupInviteBatchResult)> done) {
    facade->InviteMembersAsync(group_id, std::move(contact_ids), std::move(done));
  };
  people_picker_->BindPickerPorts(std::move(picker_ports));
  people_picker_->BindShellNavigation(shell_navigation);
  people_picker_->BindShellFeedback(shared_feedback);
  people_picker_shell_bridge_->BindApply(MakeShellChromeApplyPorts(shell, "people_picker_chrome"));
  people_picker_->BindSurfaceNotify(PeoplePickerSurfaceNotifyPorts{
      .push_surface = [this](const PeoplePickerSurfaceSnapshot& snap) { people_picker_shell_bridge_->OnSurface(snap); },
  });
  if (ui_perf_) {
    ui_perf_->BindShellNavigation(shell_navigation);
    settings_->BindOpenUiPerfTest([this]() { ui_perf_->Open(); });
    settings_->BindSimulateChatsLink([this](const std::string& state) {
      shell_->SetChatsLink(state == "online"       ? ChatsLink::Online
                           : state == "connecting" ? ChatsLink::Connecting
                                                   : ChatsLink::Offline);
    });
  }
  emoji_picker_->BindShellNavigation(shell_navigation);
  emoji_picker_->BindShellFeedback(shared_feedback);
  emoji_picker_->BindSessionStore(store_);
  client_compat_->BindCompatPorts(MakeMessagingCompatPorts(messaging));
  client_compat_->BindShellFeedback(shared_feedback);
  client_compat_->BindSupportDiscovery(SupportDiscoveryPorts{
      .on_support_changed =
          [this, &facade](const std::optional<ClientCompatSupport>& support) {
            support_discovery_ = support;
            if (facade.IsInitialized()) {
              facade.SetSupportAccountId(support && support->enabled ? support->account_id : std::string{});
            }
            if (settings_) {
              settings_->SyncSupportDiscovery();
            }
          },
  });
  badges_->BindShellNavigation(shell_navigation);
  badges_->BindAppBadge([](const int count) { SetAppBadgeCount(count); });
  badges_->BindSource([this, &facade, &shell]() {
    BadgeSourceFacts facts;
    facts.messaging_initialized = facade.IsInitialized();
    if (!facts.messaging_initialized) {
      return ComputeBadgeInputs(facts);
    }
    facts.unread_total = facade.SumUnreadIfKnown();
    // Muted chats count in the icon for now; to leave them out, set count_muted_in_icon = false and fill
    // muted_unread with the unread of the chats whose local prefs say ThreadIsMuted(muted_until, now).
    facts.sessions_tab_active = shell.State().nav_tab == NavTab::Sessions;
    if (facts.sessions_tab_active) {
      const std::string& active_id = facade.ActiveThreadId();
      if (!active_id.empty()) {
        auto thread = facade.GetThread(active_id);
        if (thread && *thread) {
          facts.open_chat_unread = (*thread)->unread_count;
        }
      }
    }
    if (contacts_shell_bridge_) {
      facts.contacts_unread = contacts_shell_bridge_->LastSurface().contacts_unread;
    }
    if (Platform::IsDesktop() && store_.Snapshot().config.mesh.node_enabled) {
      const mesh::ReachabilitySnapshot snap = facade.Reachability();
      const std::string status_key = mesh::ReachabilityStatusKey(snap.status);
      facts.me_attention =
          ReachabilityNudgeActive(true, status_key, store_.Snapshot().profile_prefs.reachability_nudge_acked_status);
    }
    return ComputeBadgeInputs(facts);
  });
  {
    BadgeNotifyPorts badge_notify;
    badge_notify.refresh = [this]() {
      if (badges_) {
        badges_->Refresh();
      }
    };
    badge_notify.sessions_unread = [this]() { return badges_ ? badges_->State().sessions_unread : 0; };
    chat_->BindBadgeNotify(std::move(badge_notify));
  }
  chat_->BindInputCoordinator(*input_);
}

void Application::WireCalls() {
  {
    CallActionsPorts call_actions;
    call_actions.start_call = [this](const std::string& thread_id, const bool want_video) {
      return call_->CallFromChat(thread_id, want_video);
    };
    call_actions.refresh_pending_ring = [this]() { call_->RefreshPendingRing(); };
    call_actions.peer_calls_ready = [this]() { return chat_->PeerCallsReady(); };
    call_actions.video_call_available = [this]() { return call_->VideoCallAvailable(); };
    call_actions.call_joinable = [this](const std::string& thread_id) {
      return call_->CallJoinableInThread(thread_id);
    };
    call_actions.call_join_bar_text = [this](const std::string& thread_id) {
      return call_->CallJoinBarText(thread_id);
    };
    call_actions.recent_calls_reader =
        [this](const size_t limit) -> std::function<std::vector<call_queries::RecentCall>()> {
      ConversationsHub& hub = Conversations();
      const std::optional<LocalIdentity> identity = messaging_facade_ ? messaging_facade_->GetIdentity() : std::nullopt;
      if (!hub.IsInitialized() || hub.ProfileDbPath().empty() || !identity || identity->account_id.empty()) {
        return {};
      }
      // Its own handle on profile.db (each call opens and closes it), so the read can run on a worker.
      return [db_path = hub.ProfileDbPath(), local = identity->account_id, limit]() {
        return call_queries::RecentCalls(SqliteCallSessionStore(db_path), local, limit);
      };
    };
    call_actions.invite_identities = [this](const std::vector<std::string>& identities) {
      call_->InviteIdentitiesToActiveCall(identities);
    };
    call_actions.start_with_invitees = [this](const std::string& thread_id, const bool video_allowed,
                                              const std::vector<std::string>& identities) {
      return call_->StartCallWithInvitees(thread_id, video_allowed, identities);
    };
    call_actions.accept_incoming = [this]() { call_->AcceptIncoming(); };
    call_actions.accept_incoming_voice_only = [this]() { call_->AcceptIncomingVoiceOnly(); };
    call_actions.accept_incoming_with_charge = [this]() { call_->AcceptIncomingWithCharge(); };
    call_actions.decline_incoming = [this]() { call_->DeclineIncoming(); };
    call_actions.leave_active = [this]() { call_->LeaveActive(); };
    call_actions.retry_connect = [this]() { call_->RetryConnect(); };
    call_actions.redial = [this]() { call_->RedialCall(); };
    call_actions.toggle_mute = [this]() { call_->ToggleMute(); };
    call_actions.toggle_camera = [this]() { call_->ToggleCamera(); };
    call_actions.flip_camera = [this]() { call_->FlipCamera(); };
    call_actions.toggle_speaker = [this]() { call_->ToggleSpeaker(); };
    call_actions.open_mid_call_invite_picker = [this]() { call_->OpenMidCallInvitePicker(); };
    call_actions.make_group_from_call = [this]() { call_->OpenMakeGroup(); };
    call_actions.join_call_group_invite = [this]() { call_->JoinGroupInvite(); };
    call_actions.put_off_call_group_invite = [this]() { call_->PutOffGroupInvite(); };
    call_actions.remove_call_guest = [this](const std::string& identity) { call_->RemoveGuestFromCall(identity); };
    call_actions.add_call_guest_to_group = [this](const std::string& identity) { call_->AddGuestToGroup(identity); };
    call_actions.may_add_to_call = [this](const std::string& identity) { return call_->MayAddToCall(identity); };
    call_actions.minimize_chrome = [this]() { call_->MinimizeChrome(); };
    call_actions.expand_chrome = [this]() { call_->ExpandChrome(); };
    call_actions.immersive_chrome = [this]() { call_->ImmersiveChrome(); };
    call_actions.spotlight_participant = [this](const std::string& stream_id) {
      call_->SpotlightParticipant(stream_id);
    };
    call_actions.spotlight_grid = [this]() { call_->SpotlightGrid(); };
    call_actions.spotlight_toggle_fill = [this]() { call_->ToggleSpotlightFill(); };
    call_actions.restore_chrome_from_minimized = [this]() { call_->RestoreChromeFromMinimized(); };
    call_actions.set_minimized_place = [this](int side, int y_permille, bool tucked) {
      call_->SetMinimizedPlace(side, y_permille, tucked);
    };
    call_actions.show_call_details = [this]() { call_->ShowCallDetails(); };
    chat_->BindCallActions(call_actions);
    contacts_->BindCallActions(call_actions);
    shell_->BindCallActions(call_actions);
    people_picker_->BindCallActions(std::move(call_actions));
  }
}

void Application::WireUnlockPinAndFlow() {
  ConversationsHub& messaging = Conversations();
  ConversationsFacade& facade = *messaging_facade_;
  unlock_gate_->BindSecrets(*secrets_);
  {
    UnlockGateCompletePorts gate_complete;
    gate_complete.complete_with_pin = [this](const SecretText& pin, const bool create_mode) {
      unlock_gate_->CompleteWithPin(pin, create_mode);
    };
    gate_complete.complete_with_default_pin = [this]() { unlock_gate_->CompleteWithDefaultPin(); };
    gate_complete.complete_link_device = [this](const SecretText& pin, const std::string& bundle_json,
                                                const bool set_default_pin) {
      unlock_gate_->CompleteLinkDevice(pin, bundle_json, set_default_pin);
    };
    gate_complete.complete_phrase_restore = [this](const SecretText& pin, const SecretText& phrase,
                                                   const bool set_default_pin,
                                                   const std::vector<std::string>& saved_copies) {
      unlock_gate_->CompletePhraseRestore(pin, phrase, set_default_pin, saved_copies);
    };
    gate_complete.pick_saved_copy = [](std::function<void(std::string, std::optional<std::string>)> picked) {
      ShowOpenFileDialog(Backend::GetWindow(), [picked = std::move(picked)](std::vector<std::string> paths) {
        std::string name;
        std::optional<std::string> text;
        if (!paths.empty()) {
          name = std::filesystem::path(paths.front()).filename().string();
          if (name.empty()) {
            name = paths.front();
          }
          text = ReadPickedFile(paths.front(), kMaxSavedAccountCopyBytes);
          ReleasePickedFile(paths.front());
        }
        AppRuntime::PostUI([picked, name = std::move(name), text = std::move(text)]() mutable {
          picked(std::move(name), std::move(text));
        });
      });
    };
    gate_complete.cancel = [this]() { unlock_gate_->Cancel(); };
    gate_complete.use_device_pin_instead = [this]() { unlock_gate_->UseDevicePinInstead(); };
    gate_complete.try_biometric = [this]() { unlock_gate_->TryBiometric(); };
    pin_gate_->BindGateComplete(std::move(gate_complete));
  }
  ProfileUnlockPorts unlock_ports;
  unlock_ports.messaging_initialized = [&messaging]() { return messaging.IsInitialized(); };
  unlock_ports.messaging_ready = [&messaging]() { return messaging.IsMessagingReady(); };
  unlock_ports.ensure_messaging_ready = [this, &messaging]() -> Roe<void> {
    auto ready = messaging.EnsureMessagingReady();
    if (ready) {
      return ready;
    }
    // The PIN was right and the vault is open, but the chat store did not open after it (newer than this build, or
    // an upgrade that failed, D105): leave the PIN screen and offer the recovery, rather than a PIN error.
    const auto failure = chat_store_gate::OpenFailure(messaging.ProfileDbPath());
    if (auto prompt = ChatStoreRecoveryFor(failure ? std::optional(failure->problem) : std::nullopt)) {
      AppRuntime::PostUI([this, prompt = *prompt]() { OfferChatStoreRecovery(prompt); });
      return {};
    }
    return ready;
  };
  unlock_ports.pin_is_default = [this]() {
    return store_.IsInitialized() && store_.Snapshot().profile_prefs.pin_is_default;
  };
  unlock_ports.set_pin_is_default = [this](const bool is_default) {
    if (!store_.IsInitialized()) {
      return;
    }
    ProfilePreferences prefs = store_.Snapshot().profile_prefs;
    prefs.pin_is_default = is_default;
    (void)store_.SaveProfilePrefs(prefs);
  };
  // The PIN's kind (B): kept from every PIN the user chose that opened the vault, so an older profile gets the number
  // pad after its next unlock and a value left wrong by an interrupted change is put right.
  unlock_ports.on_pin_verified = [this](const SecretText& pin) {
    if (!store_.IsInitialized()) {
      return;
    }
    const std::optional<int> digits = PinDigitsOf(pin.Reveal());
    ProfilePreferences prefs = store_.Snapshot().profile_prefs;
    if (!digits || prefs.pin_digits == digits) {
      return;
    }
    prefs.pin_digits = digits;
    if (auto saved = store_.SaveProfilePrefs(prefs); !saved) {
      log().warning << "PIN kind not saved: " << saved.error().message;
    }
  };
  unlock_ports.ui.show_identity_fork = [this]() { pin_gate_->ShowIdentityFork(); };
  unlock_ports.ui.show_chooser = [this]() { pin_gate_->ShowChooser(); };
  unlock_ports.ui.show_unlock = [this]() { pin_gate_->ShowUnlock(); };
  unlock_ports.ui.dismiss = [this]() {
    pin_gate_->Dismiss();
    // Any unlock but the default PIN's (grace window, PIN, dev PIN, link, recovery phrase): with the app on screen the
    // foreground timer starts now; a push cold start is in the background and does not start it.
    const bool default_pin = store_.IsInitialized() && store_.Snapshot().profile_prefs.pin_is_default;
    if (secrets_ && secrets_->IsUnlocked() && !default_pin && secrets_->GraceSupported() && os::AppShownNow()) {
      StartGraceTimer();
    }
    // The settings model was filled at start with the vault locked ("?" and no name on the Chats bar until Me opened).
    if (secrets_ && secrets_->IsUnlocked() && settings_) {
      settings_->OnProfileUnlocked();
    }
    // The PIN gate is gone and the vault is open: a fresh account continues into the first-run wizard.
    if (secrets_ && secrets_->IsUnlocked() && onboarding_) {
      onboarding_->MaybeStart();
    }
  };
  unlock_ports.ui.set_unlock_in_progress = [this](const bool in_progress) {
    pin_gate_->SetUnlockInProgressUi(in_progress);
  };
  unlock_ports.ui.show_error = [this](const std::string& message) { pin_gate_->ShowError(message); };
  unlock_ports.ui.show_waiting_device_unlock = [this](const bool waiting) {
    pin_gate_->SetWaitingDeviceUnlock(waiting);
  };
  unlock_ports.ui.on_default_provisioned = []() { UserFeedback::Ok(Tr("pin.default_provisioned_toast")); };
  // Both bring another DEK: once it is in, a face / fingerprint entry left from before could not open it (A014). A
  // failed import keeps it (the old DEK stays).
  unlock_ports.import_link_device = [this, &facade](const SecretText& pin,
                                                    const std::string& bundle_json) -> Roe<void> {
    auto imported = facade.ImportLinkDevice(bundle_json, pin);
    if (imported && secrets_) {
      secrets_->DisableBiometric();
    }
    return imported;
  };
  unlock_ports.restore_recovery_phrase = [this, &facade](const SecretText& pin, const SecretText& phrase,
                                                         const std::vector<std::string>& saved_copies) -> Roe<void> {
    auto restored = facade.RestoreFromRecoveryPhrase(phrase, pin, saved_copies);
    if (restored && secrets_) {
      secrets_->DisableBiometric();
    }
    return restored;
  };
  unlock_ports.ui.on_link_imported = [this]() {
    if (onboarding_) {
      onboarding_->MarkCompleted(); // an account brought over needs no wizard
    }
    // Import runs before EnsureMessagingReady; push attach after the vault is open. Off the UI thread, in the same
    // one-at-a-time run as the messaging-ready binding (ChatController / MessagingReadyTasks).
    if (chat_) {
      chat_->SyncPushDevices();
      chat_->ReloadAgentConfig();
    }
    UserFeedback::Ok(Tr("pin.link_imported"));
  };
  // Argon2 + libp2p stack init must not block the UI thread (Android / iOS first unlock).
  unlock_ports.run_heavy = [](std::function<Roe<void>()> work, std::function<void(Roe<void>)> on_done) {
    // Cold-start surface blips can PauseBackgroundWork without Resume; unlock must still run.
    AppRuntime::ResumeBackgroundWork();
    AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(WorkerLane::Normal, std::move(work), std::move(on_done));
  };
  // PIN grace window (#21 phase 1, DEV/PLAN-pin-grace.md).
  AttachGrace();
  unlock_ports.grace_minutes = [this]() { return GraceMinutes(); };
  unlock_ports.try_grace_unlock = [this]() { return secrets_->TryGraceUnlock(); };
  unlock_ports.remember_grace = [this](const int minutes) -> Roe<void> {
    // The timer starts with the gate's dismiss, when the app is on screen.
    auto kept = secrets_->RememberGrace(minutes);
    if (!GraceIsOn(minutes) || GraceIsAlways(minutes)) {
      StopGraceTimer(); // 0 forgets, "Always" never runs out: nothing to extend
    }
    return kept;
  };
  // Face / fingerprint unlock (at-rest A014, DEV/PLAN-pin-gate-redesign.md C): after the grace window.
  AttachBiometric();
  unlock_ports.biometric_enabled = [this]() { return secrets_ && secrets_->BiometricEnabled(); };
  unlock_ports.try_biometric_unlock = [this](const std::string& prompt) -> Roe<BiometricUnlock::Result> {
    if (!secrets_) {
      return BiometricUnlock::Result{}; // Missing: nothing to prompt for
    }
    return secrets_->TryBiometricUnlock(prompt);
  };
  unlock_ports.biometric_prompt_text = []() { return Tr("pin.biometric_prompt"); };
  unlock_ports.app_shown = []() { return os::AppShownNow(); };
  unlock_ports.on_biometric_lost = [this]() {
    if (settings_) {
      settings_->OnBiometricLost();
    }
  };
  unlock_ports.ui.show_biometric_reset = [this](const std::string& hint) { pin_gate_->ShowBiometricReset(hint); };
  unlock_ports.ui.submit_declined = [this]() { pin_gate_->OnSubmitDeclined(); };
  // A call waits for the unlock: the phone's call screen (Android), or a tapped call alert (iOS, P015).
  unlock_ports.has_pending_push_call = []() {
    return ICallSystemUi::Instance().HasPendingPushCall() ||
           PendingNotificationTap::Instance().PeekCallRoute(PendingNotificationTap::Clock::now());
  };
  unlock_ports.protected_data_available = []() { return os::ProtectedDataAvailable(); };
  unlock_ports.on_protected_data_available = [](std::function<void()> fn) {
    return os::OnProtectedDataAvailable(std::move(fn));
  };
  unlock_ports.remove_protected_data_handler = [](const uint64_t id) { os::RemoveProtectedDataHandler(id); };
  unlock_ports.run_on_ui = [](std::function<void()> fn) { AppRuntime::PostUI(std::move(fn)); };
  unlock_ports.run_later = [](const std::chrono::milliseconds delay, std::function<void()> fn) {
    AppRuntime::ScheduleCoordinatorOneShot(delay,
                                           [fn = std::move(fn)]() mutable { AppRuntime::PostUI(std::move(fn)); });
  };
  unlock_gate_->BindPorts(std::move(unlock_ports));
  // Only the foreground's paths extend the window; a push wake or a background sync starts no lifecycle event here.
  grace_foreground_listener_ = AppLifecycle::AddForegroundListener([this]() {
    unlock_gate_->OnForeground();
    TouchGrace();
    StartGraceTimer();
  });
  grace_background_listener_ = AppLifecycle::AddBackgroundListener([this]() {
    TouchGrace(); // best effort: a phone already locked keeps the entry unreadable and the touch is skipped
    StopGraceTimer();
    if (badges_) {
      badges_->Refresh();
    }
  });
  attentive_listener_ = AppLifecycle::AddAttentiveListener([this]() {
    if (badges_) {
      badges_->InvalidateAppBadge(); // the badge permission may have been granted since: send the number again
    }
    if (chat_) {
      chat_->OnAttentiveAgain();
    }
    if (badges_) {
      badges_->Refresh(); // chat_ not ready (locked, no chat): the number is still sent
    }
  });
  remote_notification_listener_ = AppLifecycle::AddRemoteNotificationListener([this]() {
    if (badges_) {
      badges_->InvalidateAppBadge(); // a push may have written its own number on the icon
      badges_->Refresh();
    }
  });

  {
    OnboardingPorts onboarding_ports;
    onboarding_ports.load_identity = [&facade]() { return facade.LoadProfileIdentityView(); };
    onboarding_ports.register_identity = [this, &facade](const std::string& nickname) {
      auto result = facade.RegisterIdentity(nickname);
      if (result && chat_) {
        chat_->ReloadAgentConfig();
      }
      return result;
    };
    onboarding_ports.run_heavy = [](std::function<Roe<void>()> work, std::function<void(Roe<void>)> on_done) {
      AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(WorkerLane::Normal, std::move(work), std::move(on_done));
    };
    onboarding_ports.share_text = [](const std::string& text, const ShareAnchor anchor) {
      return PlatformShareText(Backend::GetWindow(), text, anchor);
    };
    onboarding_ports.copy_text = [](const std::string& text) {
      if (ui::SystemInterface* system = ui::GetSystemInterface()) {
        system->SetClipboardText(text.c_str());
      }
    };
    onboarding_ports.find_friends = [this]() {
      if (contacts_) {
        contacts_->OnFindSomeone();
      }
    };
    onboarding_ports.is_completed = [this]() {
      return store_.IsInitialized() && store_.Snapshot().profile_prefs.onboarding_completed;
    };
    onboarding_ports.set_completed = [this](const bool completed) {
      if (!store_.IsInitialized()) {
        return;
      }
      ProfilePreferences prefs = store_.Snapshot().profile_prefs;
      if (prefs.onboarding_completed == completed) {
        return;
      }
      prefs.onboarding_completed = completed;
      prefs.schema_version = ProfilePreferences::kSchemaVersion;
      (void)store_.SaveProfilePrefs(prefs);
    };
    onboarding_ports.mount = [this](const std::string& body) {
      if (shell_) {
        shell_->MountOverlayBody("shell-onboarding-mount", body);
      }
    };
    onboarding_->BindPorts(std::move(onboarding_ports));
  }

  {
    UnlockEnsurePorts unlock_ensure;
    unlock_ensure.ensure_unlocked = [this](std::function<void(bool)> done) {
      unlock_gate_->EnsureUnlocked(std::move(done));
    };
    unlock_ensure.is_unlock_in_progress = [this]() { return unlock_gate_->IsUnlockInProgress(); };
    chat_->BindUnlockEnsure(unlock_ensure);
    settings_->BindUnlockEnsure(unlock_ensure);
    // B2: the transfer code, the recovery phrase and Change PIN ask for the PIN on the gate (its pad for a digit PIN).
    PinPromptPorts pin_prompt;
    pin_prompt.show_verify = [this](const PinVerifyPurpose purpose, PinGateCheck check, std::function<void()> on_ok) {
      pin_gate_->ShowVerify(purpose, std::move(check), std::move(on_ok));
    };
    pin_prompt.show_change = [this](PinGateCheck check_old, PinGateChange change,
                                    std::optional<SecretText> current_pin) {
      pin_gate_->ShowChange(std::move(check_old), std::move(change), std::move(current_pin));
    };
    settings_->BindPinPrompt(std::move(pin_prompt));
    contacts_->BindUnlockEnsure(unlock_ensure);
    people_picker_->BindUnlockEnsure(std::move(unlock_ensure));
  }
  // Invite links the OS opens the app with (QR codes, tapped links); those from a cold start waited.
  IncomingLink::SetHandler([this](const std::string& link) {
    if (chat_ && chat_->OfferGroupJoin(link)) {
      return; // a group's invite link (G021, pp://join/…)
    }
    if (contacts_) {
      contacts_->OfferInvite(link);
    }
  });
  contacts_->BindGroupJoinOffer([this](const std::string& text) { return chat_ && chat_->OfferGroupJoin(text); });
  {
    MessagingShellPorts shell_messaging = MakeMessagingShellPorts(messaging);
    shell_messaging.open_network_settings = [this]() {
      if (settings_) {
        settings_->OpenNetworkSettings();
      }
    };
    shell_->BindShellMessaging(std::move(shell_messaging));
  }
  {
    PinGateActionPorts pin_actions;
    pin_actions.on_submit = [this]() { pin_gate_->OnSubmit(); };
    pin_actions.on_cancel = [this]() { pin_gate_->OnCancel(); };
    pin_actions.on_set_pin = [this]() { pin_gate_->OnSetPin(); };
    pin_actions.on_use_default = [this]() { pin_gate_->OnUseDefaultPin(); };
    pin_actions.on_identity_new = [this]() { pin_gate_->OnIdentityNew(); };
    pin_actions.on_identity_link = [this]() { pin_gate_->OnIdentityLink(); };
    pin_actions.on_identity_phrase = [this]() { pin_gate_->OnIdentityPhrase(); };
    pin_actions.on_use_saved_copy = [this]() { pin_gate_->OnUseSavedCopy(); };
    pin_actions.on_forgot_toggle = [this]() { pin_gate_->OnForgotToggle(); };
    pin_actions.on_use_pin_instead = [this]() { pin_gate_->OnUsePinInstead(); };
    pin_actions.on_key = [this](const int digit) { pin_gate_->OnKey(digit); };
    pin_actions.on_backspace = [this]() { pin_gate_->OnBackspace(); };
    pin_actions.on_keypad_ok = [this]() { pin_gate_->OnKeypadOk(); };
    pin_actions.on_use_text = [this]() { pin_gate_->OnUseText(); };
    pin_actions.on_use_keypad = [this]() { pin_gate_->OnUseKeypad(); };
    pin_actions.on_biometric = [this]() { pin_gate_->OnBiometric(); };
    pin_actions.on_use_pin = [this]() { pin_gate_->OnUsePin(); };
    shell_->BindPinGateActions(std::move(pin_actions));
  }
  {
    FlowCoordinatorPorts flow_ports;
    flow_ports.begin_modal = [this](int layer_id, std::function<bool()> on_step_back, std::function<void()> on_cancel) {
      flow_->BeginModal(layer_id, std::move(on_step_back), std::move(on_cancel));
    };
    flow_ports.end_modal = [this]() { flow_->EndModal(); };
    flow_ports.handle_dismiss = [this]() { return flow_->HandleDismiss(); };
    flow_ports.notify_layer_closing = [this](int layer_id) { flow_->NotifyLayerClosing(layer_id); };
    shell_->BindFlowCoordinator(flow_ports);
    people_picker_->BindFlowCoordinator(flow_ports);
    contacts_->BindFlowCoordinator(flow_ports);
    chat_->BindFlowCoordinator(flow_ports);
    qr_scan_->BindFlowCoordinator(flow_ports);
    avatar_crop_->BindFlowCoordinator(flow_ports);
    avatar_view_->BindFlowCoordinator(flow_ports);
    emoji_picker_->BindFlowCoordinator(std::move(flow_ports));
  }
}

void Application::WireAgentAndConfig() {
  ConversationsHub& messaging = Conversations();
  config_apply_->Bind(messaging, store_, *shell_, *chat_,
                      [](const std::string& relative) { return AssetsPath(relative); });

  agent_sessions_.emplace();
  chat_->BindAgentPorts(MakeAgentUiPorts(*agent_sessions_));
  if (messaging_facade_) {
    // B183: the gateway refused the AI Bearer (HTTP 401). A guest key is minted and stored beside the registered key
    // (never re-registers); the refused key goes only once a retry with the new one was answered. Runs on the turn's
    // thread: the identity store locks itself (the diagnostics Bearer does the same off the UI); the sessions take the
    // new key once no turn runs.
    ConversationsFacade& facade = *messaging_facade_;
    const BriefLlmKeyStore key_store = MakeBriefLlmKeyStore(facade);
    // The renewal shares the Brief key run with the unlock's guest-key mint (ChatController::BriefKeyRun): while that
    // mint is in flight the turn waits for its key (stored, then reused) rather than minting a second one.
    agent_sessions_->SetBriefKeyPorts(BriefKeyPorts{
        .renew = [this, key_store, run = chat_->BriefKeyRun()](
                     const std::string& rejected, const std::string& llm_base_url,
                     const std::atomic<bool>& cancelled) -> Roe<BriefKeyRenewal> {
          // A stopped turn (Stop, profile reset, quit: StopTurnsAndWait / ~AgentSessions cancel it) gives up waiting
          // for another run's mint within a slice. A mint of its own (RenewBriefLlmKey) is not interrupted: it runs to
          // its HTTP limit (30 s), as before the run was shared.
          const std::chrono::milliseconds wait =
              AppRuntime::CurrentlyOnUI() ? std::chrono::milliseconds(0) : kBriefKeyMintWait;
          auto waited = RunSharedBriefKeyRenewal(
              *run, wait, [&cancelled]() { return cancelled.load(); },
              [&]() { return RenewBriefLlmKey(key_store, rejected, llm_base_url); },
              [this]() { AppRuntime::PostUI([this] { chat_->RetryBriefKeyMint(); }); });
          if (!waited) {
            return Error("Brief key renewal stopped: the turn was cancelled");
          }
          auto& renewed = *waited;
          if (!renewed) {
            return renewed.error();
          }
          return BriefKeyRenewal{.key = std::move(renewed->key), .reused = renewed->reused};
        },
        .commit =
            [this, key_store](const std::string& rejected) {
              if (auto committed = CommitBriefLlmKey(key_store, rejected); !committed) {
                log().warning << "Brief LLM key: the refused key was not dropped: " << committed.error().message;
              }
              AppRuntime::PostUI([this] { chat_->ReloadAgentConfigWhenIdle(); });
            },
        .stored_key = [key_store]() { return StoredBriefLlmKey(key_store); },
    });
  }
  if (messaging_facade_) {
    // MessageRouter checks is_busy before it routes, so these start; anything else would be a send that slipped past.
    const auto log_not_started = [this](const SubmitOutcome outcome) {
      if (outcome != SubmitOutcome::Started) {
        log().warning << "AI question from the router not started (outcome " << static_cast<int>(outcome) << ")";
      }
    };
    AgentInboundPorts inbound;
    inbound.submit_scoped_assist = [this, log_not_started](const std::string& thread_id, const std::string& prompt,
                                                           std::optional<std::string> user_payload, AtAiMode mode) {
      if (agent_sessions_) {
        log_not_started(agent_sessions_->SubmitScopedAssist(thread_id, prompt, std::move(user_payload), mode));
      }
    };
    // Asked before anything is stored or sent: a turn already runs in that thread, or as many as allowed run.
    inbound.is_busy = [this](const std::string& thread_id) {
      return agent_sessions_ && agent_sessions_->CanStart(thread_id) != TurnStart::Yes;
    };
    inbound.submit_to_thread = [this, log_not_started](const std::string& thread_id, const std::string& user_text,
                                                       std::optional<std::string> user_payload) {
      if (agent_sessions_) {
        log_not_started(agent_sessions_->SubmitToThread(thread_id, user_text, std::move(user_payload)));
      }
    };
    // Buttons of the on-device app-action flow ({"type":"local_action",...}) run in the session, no model.
    inbound.submit_local_action = [this, log_not_started](const std::string& thread_id, const std::string& text,
                                                          const std::string& payload, const bool in_peer_chat) {
      const auto doc = TryParseObject(payload);
      if (!agent_sessions_ || !doc) {
        return;
      }
      Object args;
      for (const auto& [key, value] : doc->fields()) {
        if (key != "type" && key != "tool") {
          args.set(key, value);
        }
      }
      log_not_started(agent_sessions_->SubmitLocalAction(thread_id, text, doc->getString("tool").value_or(""),
                                                         std::move(args), in_peer_chat));
    };
    inbound.is_app_action_request = [this](const std::string& text) {
      return agent_sessions_ && agent_sessions_->IsAppActionRequest(text);
    };
    messaging_facade_->BindAgentInbound(std::move(inbound));

    // The planner matches names against the user's remark and the other side's nickname, on the device only.
    // Called on a worker (like the messaging tools): the contacts store and the thread store lock themselves.
    agent_sessions_->SetLocalContactsSource([this]() {
      std::vector<LocalPlanContact> out;
      if (!messaging_facade_) {
        return out;
      }
      auto contacts = messaging_facade_->SearchLocalContacts("");
      if (!contacts) {
        return out;
      }
      std::map<std::string, std::pair<std::string, int64_t>> direct_threads; // contact id -> (thread id, updated)
      if (auto threads = messaging_facade_->ListThreads()) {
        for (const Thread& thread : *threads) {
          if (thread.kind == ThreadKind::Direct && thread.participant_contact_ids.size() == 1) {
            direct_threads[thread.participant_contact_ids.front()] = {thread.id, thread.updated_at};
          }
        }
      }
      out.reserve(contacts->size());
      for (const Contact& contact : *contacts) {
        LocalPlanContact flat{
            .id = contact.id, .remark = contact.local.display_name, .nickname = contact.remote.nickname};
        if (const auto it = direct_threads.find(contact.id); it != direct_threads.end()) {
          flat.thread_id = it->second.first;
          flat.last_contact_ms = it->second.second;
        }
        out.push_back(std::move(flat));
      }
      return out;
    });
  }
  if (agent_sessions_) {
    agent_sessions_->SetToolPermissions(store_.Snapshot().profile_prefs.tool_permissions);
    agent_sessions_->SetToolPermissionsSaver([this](const ToolPermissionsPrefs& permissions) -> Roe<void> {
      ProfilePreferences prefs = store_.Snapshot().profile_prefs;
      prefs.tool_permissions = permissions;
      prefs.schema_version = ProfilePreferences::kSchemaVersion;
      return store_.SaveProfilePrefs(prefs);
    });
    store_.AddProfilePrefsListener([this](const ProfilePreferences& prefs) {
      if (agent_sessions_) {
        agent_sessions_->SetToolPermissions(prefs.tool_permissions);
      }
    });
  }
  contacts_->SetShowAdvanced(store_.Snapshot().profile_prefs.show_advanced_settings);
  shell_->SetShowAdvancedSettings(store_.Snapshot().profile_prefs.show_advanced_settings);
  store_.AddProfilePrefsListener([this](const ProfilePreferences& prefs) {
    contacts_->SetShowAdvanced(prefs.show_advanced_settings);
    shell_->SetShowAdvancedSettings(prefs.show_advanced_settings);
  });
  // A contact starred or unstarred (on their page), the recovery phrase opened, or the 动态 section toggled: Home follows.
  store_.AddProfilePrefsListener(
      [this, starred = store_.Snapshot().profile_prefs.starred_contacts,
       phrase_seen = store_.Snapshot().profile_prefs.recovery_phrase_seen,
       show_dynamic = store_.Snapshot().profile_prefs.home_show_dynamic](const ProfilePreferences& prefs) mutable {
        if (prefs.starred_contacts != starred || prefs.recovery_phrase_seen != phrase_seen ||
            prefs.home_show_dynamic != show_dynamic) {
          starred = prefs.starred_contacts;
          phrase_seen = prefs.recovery_phrase_seen;
          show_dynamic = prefs.home_show_dynamic;
          chat_->OnHomePrefsChanged();
        }
      });
}

bool Application::MountPresenters(ui::Context* context) {
  if (!settings_->RegisterModel(context)) {
    log().error << "SettingsController RegisterModel failed";
    return false;
  }

  if (!contacts_->RegisterModel(context)) {
    log().error << "ContactsController RegisterModel failed";
    return false;
  }

  if (!qr_scan_->RegisterModel(context)) {
    log().error << "QrScanController RegisterModel failed";
    return false;
  }

  if (!avatar_crop_->RegisterModel(context)) {
    log().error << "AvatarCropController RegisterModel failed";
    return false;
  }

  if (!avatar_view_->RegisterModel(context)) {
    log().error << "AvatarViewController RegisterModel failed";
    return false;
  }

  if (!people_picker_->RegisterModel(context)) {
    log().error << "PeoplePickerController RegisterModel failed";
    return false;
  }

  if (!emoji_picker_->RegisterModel(context)) {
    log().error << "EmojiPickerController RegisterModel failed";
    return false;
  }
  if (ui_perf_ && !ui_perf_->RegisterModel(context)) {
    log().error << "UiPerfTestController RegisterModel failed";
    return false;
  }
  if (!onboarding_->RegisterModel(context)) {
    log().error << "OnboardingController RegisterModel failed";
    return false;
  }

  if (!shell_->RegisterWindowModel(context)) {
    log().error << "ShellHost RegisterWindowModel failed";
    return false;
  }

  ContactsNotifyPorts contacts_notify;
  contacts_notify.refresh = [this]() { contacts_->Refresh(); };
  contacts_notify.select_contact = [this](const std::string& id) { contacts_->OnSelectContact(id); };
  contacts_notify.select_contact_sheet = [this](const std::string& id) { contacts_->OnSelectContactSheet(id); };
  contacts_notify.show_member_sheet = [this](const std::string& account_id, const std::string& name,
                                             const std::string& icon_src, const bool from_dm) {
    contacts_->OnShowMemberSheet(account_id, name, icon_src, from_dm);
  };
  contacts_notify.set_group_admin = [this](GroupAdminCard card) { contacts_->SetGroupAdminCard(std::move(card)); };
  contacts_notify.open_new_chat = [this]() { contacts_->OpenNewChatPage(); };
  contacts_notify.message_contact = [this](const std::string& id) { contacts_->MessageContact(id); };
  // The member page opened from a 1:1 chat: its Block and Add are the chat menu's own (ChatPresenter).
  contacts_->BindStrangerBlocker([this](const std::string& account_id) { return chat_->BlockStranger(account_id); });
  contacts_->BindStrangerAdder([this](const std::string& account_id) { return chat_->AddStranger(account_id); });
  people_picker_->BindContactsNotify(contacts_notify); // a member's profile from the member list
  chat_->BindContactsNotify(std::move(contacts_notify));

  PeoplePickerNotifyPorts chat_people_picker_notify;
  chat_people_picker_notify.open_free = [this]() { people_picker_->OpenFree(); };
  chat_people_picker_notify.open_from_dm = [this](const std::string& id) { people_picker_->OpenFromDm(id); };
  chat_people_picker_notify.open_group_members = [this](const std::string& thread_id) {
    people_picker_->OpenForGroupMembers(thread_id);
  };
  chat_people_picker_notify.open_group_add_members = [this](const std::string& thread_id) {
    people_picker_->OpenForGroupAddMembers(thread_id);
  };
  chat_people_picker_notify.tap_group_member = [this](const std::string& thread_id, const std::string& identity) {
    people_picker_->TapGroupMember(thread_id, identity);
  };
  chat_people_picker_notify.offer_group_admin = [this](const std::string& thread_id, const std::string& sender) {
    people_picker_->OfferGroupAdmin(thread_id, sender);
  };
  chat_->BindPeoplePickerNotify(std::move(chat_people_picker_notify));

  EmojiPickerNotifyPorts emoji_notify;
  emoji_notify.open_insert = [this]() {
    emoji_picker_->OpenInsert(
        [this](std::string emoji, bool restore_focus) { chat_->InsertEmojiIntoDraft(emoji, restore_focus); });
  };
  emoji_notify.open_insert_into = [this](std::function<void(const std::string& emoji)> insert) {
    emoji_picker_->OpenInsert([insert = std::move(insert)](std::string emoji, bool) { insert(emoji); });
  };
  emoji_notify.open_react = [this](const std::string& message_id) {
    emoji_picker_->OpenReact(message_id,
                             [this, message_id](std::string emoji) { chat_->ReactWithEmoji(message_id, emoji); });
  };
  emoji_notify.close_if_open = [this]() {
    if (emoji_picker_->IsOpen()) {
      emoji_picker_->Close();
    }
  };
  chat_->BindEmojiPickerNotify(std::move(emoji_notify));

  PeoplePickerNotifyPorts call_people_picker_notify;
  call_people_picker_notify.open_for_group_call = [this](const std::string& thread_id, bool video_allowed) {
    people_picker_->OpenForGroupCall(thread_id, video_allowed);
  };
  call_people_picker_notify.open_for_call_add_guest = [this](const std::string& call_id) {
    people_picker_->OpenForCallAddGuest(call_id);
  };
  call_people_picker_notify.open_for_call_make_group = [this](const std::string& call_id) {
    people_picker_->OpenForCallMakeGroup(call_id);
  };
  call_->BindPeoplePickerNotify(std::move(call_people_picker_notify));

  if (![&] {
        StartupPhase phase("SetupChatController");
        return chat_->Setup(context);
      }()) {
    log().error << "SetupChatController failed";
    settings_->BindCommands({});
    settings_->BindShellNavigation({});
    settings_->BindShellFeedback({});
    contacts_->BindShellNavigation({});
    contacts_->BindShellFeedback({});
    contacts_->BindSurfaceNotify({});
    if (contacts_shell_bridge_) {
      contacts_shell_bridge_->Clear();
    }
    contacts_->BindContactsPorts({});
    chat_->BindRefreshContact({});
    contacts_->BindSessionStore(nullptr);
    settings_->BindOpenHomeEdit({});
    chat_->BindShellNavigation({});
    chat_->BindShellFeedback({});
    chat_->BindSurfaceNotify({});
    if (chat_shell_bridge_) {
      chat_shell_bridge_->Clear();
    }
    chat_->BindShellSetup({});
    chat_->BindConversationsFacade(nullptr);
    chat_->BindAgentPorts({});
    if (messaging_facade_) {
      messaging_facade_->BindAgentInbound({});
    }
    chat_->BindContactsNotify({});
    people_picker_->BindContactsNotify({});
    chat_->BindPeoplePickerNotify({});
    chat_->BindEmojiPickerNotify({});
    chat_->BindMessagingUi({});
    UserFeedback::BindPorts({});
    ShellFeedback::BindChromePorts({});
    contacts_->BindChatPorts({});
    people_picker_->BindChatPorts({});
    people_picker_->BindShellNavigation({});
    people_picker_->BindShellFeedback({});
    people_picker_->BindSurfaceNotify({});
    if (people_picker_shell_bridge_) {
      people_picker_shell_bridge_->Clear();
    }
    people_picker_->BindContactsPorts({});
    people_picker_->BindPickerPorts({});
    if (emoji_picker_) {
      emoji_picker_->BindShellNavigation({});
      emoji_picker_->BindShellFeedback({});
      emoji_picker_->BindFlowCoordinator({});
    }
    if (ui_perf_) {
      settings_->BindOpenUiPerfTest({});
      settings_->BindSimulateChatsLink({});
      ui_perf_->BindShellNavigation({});
    }
    if (client_compat_) {
      client_compat_->BindCompatPorts({});
      client_compat_->BindShellFeedback({});
      client_compat_->BindSupportDiscovery({});
    }
    if (flow_) {
      flow_->BindShellNavigation({});
    }
    if (badges_) {
      badges_->BindSource({});
      badges_->BindShellNavigation({});
    }
    if (pin_gate_) {
      pin_gate_->BindShellPinGate({});
      pin_gate_->BindGateComplete({});
      pin_gate_->BindKeypad({});
      if (onboarding_) {
        onboarding_->BindPorts({});
      }
    }
    if (call_) {
      call_->BindCallPorts({});
      call_->BindShellCallChrome({});
      call_->BindPeerLinkRefresh(nullptr);
      call_->BindPeoplePickerNotify({});
    }
    if (contacts_) {
      contacts_->BindCallActions({});
      contacts_->BindStrangerBlocker(nullptr);
      contacts_->BindStrangerAdder(nullptr);
    }
    if (chat_) {
      chat_->BindCallActions({});
      chat_->BindBadgeNotify({});
      chat_->BindUnlockEnsure({});
    }
    if (shell_) {
      shell_->BindCallActions({});
      shell_->BindPinGateActions({});
      shell_->BindFlowCoordinator({});
    }
    if (people_picker_) {
      people_picker_->BindCallActions({});
      people_picker_->BindUnlockEnsure({});
      people_picker_->BindFlowCoordinator({});
    }
    if (emoji_picker_) {
      emoji_picker_->BindFlowCoordinator({});
    }
    if (settings_) {
      settings_->BindUnlockEnsure({});
      settings_->BindPinPrompt({});
    }
    if (contacts_) {
      contacts_->BindUnlockEnsure({});
    }
    call_ui_.reset();
    if (unlock_gate_) {
      unlock_gate_->BindPorts({});
    }
    agent_sessions_.reset();
    ui::RemoveContext("main");
    ui::Shutdown();
    Backend::Shutdown();
    return false;
  }
  return true;
}

namespace {

/** UI demo app only: fixed fake data, written once per process before any controller reads the stores. */
bool SeedUiDemoDataOnce(ConversationsHub& messaging, SessionStore& store) {
  static DemoDataSeeder seeder;
  auto log = logging::getLogger("Application");
  auto deps = MakeDemoSeedDeps(messaging);
  if (!deps) {
    log.error << "Demo data not written: " << deps.error().message;
    return false;
  }
  // Home's widgets: missed calls in profile.db, and a couple of starred contacts when none is starred.
  const SqliteCallSessionStore calls(messaging.ProfileDbPath());
  deps->calls = &calls;
  deps->star_contacts = [&store, &log](const std::vector<std::string>& ids) {
    ProfilePreferences prefs = store.Snapshot().profile_prefs;
    if (!prefs.starred_contacts.empty()) {
      return;
    }
    prefs.schema_version = ProfilePreferences::kSchemaVersion;
    prefs.starred_contacts = ids;
    if (auto saved = store.SaveProfilePrefs(prefs); !saved) {
      log.warning << "Demo stars not saved: " << saved.error().message;
    }
  };
  auto seeded = seeder.SeedOnce(*deps);
  if (!seeded) {
    log.error << "Demo data not written: " << seeded.error().message;
    return false;
  }
  return *seeded;
}

} // namespace

void Application::SeedAndRefreshUiDemo() {
  if (!SeedUiDemoDataOnce(Conversations(), store_)) {
    return; // already seeded, or failed (logged as an error by the seeder wrapper)
  }
  // The lists may already have loaded the empty stores: tell them (and the tab badges) about the new rows.
  chat_->OnMessagingReady();
  contacts_->Refresh();
  if (messaging_facade_) {
    messaging_facade_->NotifyThreadChanged();
  }
  if (badges_) {
    badges_->Refresh();
  }
}

void Application::WireHubCallbacks() {
  ConversationsHub& messaging = Conversations();
  const auto on_ready = [this]() {
    if constexpr (kUiDemoBuild) {
      SeedAndRefreshUiDemo(); // first and synchronous: chat_ and contacts_ read the rows right after
    }
    // at-rest A011 (temporary): the Keychain backup item earlier builds wrote; nothing reads it now.
    AppRuntime::PostWorkerNormal([this]() {
      if (auto removed = RemoveRetiredKeychainBackup(OsSecureBlobStore()); !removed) {
        log().warning << "Retired Keychain backup not removed: " << removed.error().message;
      }
    });
    {
      StartupPhase phase("ChatController::OnMessagingReady");
      chat_->OnMessagingReady();
    }
    {
      StartupPhase phase("CallController::OnMessagingReady");
      call_->OnMessagingReady();
    }
    contacts_->Refresh();
    // A chat database too old to upgrade was reset when it opened (D105): say so once.
    if (TakeChatStoreResetNotice(store_.Snapshot().profile_data_dir)) {
      UserFeedback::Note(Tr("settings.storage.chats_reset_too_old"));
    }
    if (messaging_facade_ && support_discovery_ && support_discovery_->enabled) {
      messaging_facade_->SetSupportAccountId(support_discovery_->account_id);
    }
    const ShellChromeSnapshot chrome = ProjectShellChromeSnapshot(shell_->State());
    if (chrome.nav_tab == NavTab::Me) {
      settings_->OnNavTabActivated();
    }
  };
  messaging.SetOnMessagingReady(on_ready);
  // The demo app unlocks at bootstrap, before this callback exists, so its ready event was already missed. The
  // controllers are not mounted yet and read the stores when they are: seeding now is early enough.
  if constexpr (kUiDemoBuild) {
    if (messaging.IsMessagingReady()) {
      SeedAndRefreshUiDemo();
    }
  }
  messaging.SetOnMeshReady([this]() {
    chat_->OnMeshReady();
    contacts_->Refresh();
    if (badges_) {
      badges_->Refresh();
    }
  });
  messaging.SetOnCallWake([this]() {
    if (call_) {
      call_->OnCallWake();
    }
  });
  messaging.SetOnReachabilityUpdated([this]() {
    AppRuntime::PostUI([this]() {
      const mesh::ReachabilitySnapshot snap = messaging_facade_->Reachability();
      if (snap.status == mesh::ReachabilityStatus::Reachable) {
        ProfilePreferences prefs = store_.Snapshot().profile_prefs;
        if (!prefs.reachability_nudge_acked_status.empty()) {
          prefs.reachability_nudge_acked_status.clear();
          prefs.schema_version = ProfilePreferences::kSchemaVersion;
          (void)store_.SaveProfilePrefs(prefs);
        }
      }
      settings_->SyncReachability();
      if (badges_) {
        badges_->Refresh();
      }
    });
  });
  // The Chats header's status line: the OS monitor reports on its own thread, the shell updates on the UI thread.
  // Network + relay poll are combined in the hub (#175); this fires on a change from the monitor / poll / timer threads.
  messaging.SetOnLinkStateChanged([this](const ConversationsHub::LinkState state) {
    AppRuntime::PostUI([this, state]() {
      if (!shell_) {
        return;
      }
      switch (state) {
        case ConversationsHub::LinkState::Online:
          shell_->SetChatsLink(ChatsLink::Online);
          break;
        case ConversationsHub::LinkState::Connecting:
          shell_->SetChatsLink(ChatsLink::Connecting);
          break;
        case ConversationsHub::LinkState::Offline:
          shell_->SetChatsLink(ChatsLink::Offline);
          break;
        case ConversationsHub::LinkState::Unknown:
          shell_->SetChatsLink(ChatsLink::Unknown);
          break;
      }
    });
  });
  messaging.SetOnPeerIconsChanged([this]() {
    // A burst of icons (a group's members) is one refresh: the flag is cleared when the posted task runs.
    if (peer_icons_refresh_posted_.exchange(true)) {
      return;
    }
    AppRuntime::PostUI([this]() {
      peer_icons_refresh_posted_ = false;
      if (contacts_) {
        contacts_->Refresh();
        contacts_->RefreshMemberIcon(
            [this](const std::string& identity) { return Conversations().IdentityIconLocalPath(identity); });
      }
      if (chat_) {
        chat_->OnPeerIconsChanged();
      }
      if (call_) {
        call_->RefreshPendingRing();
      }
    });
  });
}

void Application::WireHubLifecycle(ui::Context* context, const BootstrapResult& bootstrap) {
  ShellHost& shell = *shell_;
  ChatSessionPorts chat_ports;
  chat_ports.finalize_thread_display = [this]() { chat_->FinalizeThreadDisplay(); };
  chat_ports.select_thread = [this](const std::string& id) { chat_->OnSelectThread(id); };
  chat_ports.on_find_someone = [this]() { chat_->OnFindSomeone(); };
  chat_ports.open_new_group = [this]() { people_picker_->OpenFree(); };
  chat_ports.new_ai_chat = [this]() { chat_->StartNewAiChat(); };
  contacts_->BindChatPorts(chat_ports);
  people_picker_->BindChatPorts(std::move(chat_ports));

  WireShellPresentationEvents(shell, badges_.get(), *settings_, *contacts_, *chat_, ui_perf_.get());
  shell_->SetOnBottomChromeDismissed([this]() {
    if (emoji_picker_) {
      emoji_picker_->Close();
    }
  });
  // Escape (not the system Back) in the composer: cancel this chat's queued AI question / stop its turn (B112).
  PlatformNavigation::SetEscapeKeyHandler([this]() {
    switch (chat_->OnComposerEscape()) {
      case ChatController::ComposerEscape::Handled:
        return 1;
      case ChatController::ComposerEscape::Idle:
        return 2;
      case ChatController::ComposerEscape::NotInComposer:
        break;
    }
    return 0;
  });

  // After chat exists: fan-out config/prefs, then own hub lifecycle callbacks (not ChatController).
  config_apply_->InstallListeners();

  WireHubCallbacks();

  LocalizationService::Instance().AddLanguageChangeListener([this](const std::string& /*resolved*/) {
    settings_->RefreshLocalizedChrome();
    ViewCatalog::ClearCache();
    shell_->RequestSyncLayout(true);
    if (auto* ctx = ui::GetContext("main")) {
      ApplyUiDocumentLanguage(ctx);
    }
  });

  shell_->SetSafeAreaInsetsFromPrefs(bootstrap.machine_prefs.safe_area.top, bootstrap.machine_prefs.safe_area.bottom);
  shell_->RefreshSafeAreaInsets(context);
  shell_->SyncChromeMaterialPrefs(bootstrap.profile_prefs.reduce_transparency,
                                  bootstrap.profile_prefs.compact_chrome_frost);
  shell_->SetSidebarPrefs(bootstrap.profile_prefs.sidebar_width_dp, bootstrap.profile_prefs.sidebar_collapsed);
  shell_->SetOnSidebarChanged([this](int width_dp, bool collapsed) {
    ProfilePreferences prefs = store_.Snapshot().profile_prefs;
    if (prefs.sidebar_width_dp == width_dp && prefs.sidebar_collapsed == collapsed) {
      return;
    }
    prefs.sidebar_width_dp = width_dp;
    prefs.sidebar_collapsed = collapsed;
    prefs.schema_version = ProfilePreferences::kSchemaVersion;
    (void)store_.SaveProfilePrefs(prefs);
  });
  // Phone AI dot (DESIGN-pp-ui §8.14): on / look / size follow Me → AI assistant through ConfigApplyBridge; where it rests
  // is written here after a drag. A tap opens the ask-AI panel; it keeps off Me → Account & security.
  shell_->SetAiDot(bootstrap.profile_prefs.ai_dot_shown, bootstrap.profile_prefs.ai_dot_look,
                   bootstrap.profile_prefs.ai_dot_size);
  shell_->SetAiDotPlace(bootstrap.profile_prefs.ai_dot_left, bootstrap.profile_prefs.ai_dot_y_permille,
                        bootstrap.profile_prefs.ai_dot_tucked);
  shell_->SetOnAiDotTap([this]() { chat_->OpenAskAiFromDot(); });
  shell_->SetOnAiDotMoved([this](const bool left, const int y_permille, const bool tucked) {
    ProfilePreferences prefs = store_.Snapshot().profile_prefs;
    if (prefs.ai_dot_left == left && prefs.ai_dot_y_permille == y_permille && prefs.ai_dot_tucked == tucked) {
      return;
    }
    prefs.ai_dot_left = left;
    prefs.ai_dot_y_permille = y_permille;
    prefs.ai_dot_tucked = tucked;
    prefs.schema_version = ProfilePreferences::kSchemaVersion;
    (void)store_.SaveProfilePrefs(prefs);
  });
  shell_->SetSecretPage(
      [this]() { return shell_->State().nav_tab == NavTab::Me && settings_->ShowsSecuritySection(); });
  shell_->SetSecretLegible(
      [this]() { return shell_->State().nav_tab == NavTab::Me && settings_->ShowsRecoveryPhrase(); });
  shell_->SetAuxiliaryWidthPref(bootstrap.profile_prefs.auxiliary_width_dp);
  shell_->SetOnAuxiliaryWidthChanged([this](int width_dp) {
    ProfilePreferences prefs = store_.Snapshot().profile_prefs;
    if (prefs.auxiliary_width_dp == width_dp) {
      return;
    }
    prefs.auxiliary_width_dp = width_dp;
    prefs.schema_version = ProfilePreferences::kSchemaVersion;
    (void)store_.SaveProfilePrefs(prefs);
  });

  ApplyUiDocumentLanguage(context);
}

bool Application::Initialize(const char* window_title) {
  if (initialized_) {
    return true;
  }

  if (!store_.IsInitialized()) {
    log().error << "SessionStore not initialized";
    return false;
  }

  const BootstrapResult& bootstrap = store_.Snapshot();

  const int width = bootstrap.machine_prefs.window.width;
  const int height = bootstrap.machine_prefs.window.height;

  int window_width = width;
  int window_height = height;
  if (Platform::IsMobile()) {
    ResolveMobileWindowSize(window_width, window_height);
  }

  log().info << "Initializing (" << window_width << "x" << window_height << ")";

  ui::Context* context = nullptr;
  if (!InitializeUiHost(window_title, window_width, window_height, bootstrap, context)) {
    return false;
  }

  const SettingsToolPorts settings_tool_ports = WireSettings(context);
  WireShellPresenters(settings_tool_ports);
  WireCalls();
  WireUnlockPinAndFlow();
  WireAgentAndConfig();
  if (!MountPresenters(context)) {
    return false;
  }
  WireHubLifecycle(context, bootstrap);

  log().info << "Initialization complete";
  initialized_ = true;
  return true;
}

void Application::Run() {
  if (!initialized_) {
    return;
  }

  auto* context = ui::GetContext("main");
  if (!context) {
    return;
  }

  int skip_log_countdown = 0;
  bool logged_first_present = false;
  // B79: a launch that never presents its first frame (black screen) left no trace. If nothing was
  // presented 8 s after entering the loop, every thread prints its stack to stderr.
  auto first_present_seen = std::make_shared<std::atomic<bool>>(false);
  std::thread([first_present_seen]() {
    std::this_thread::sleep_for(std::chrono::seconds(8));
    if (first_present_seen->load()) {
      return;
    }
    logging::getLogger("startup").error << "no first present 8 s after entering the run loop; dumping thread stacks";
    os::DumpAllThreadStacks();
  }).detach();
  constexpr int64_t kSlowFrameMs = 50; // three 60 Hz frames
  auto last_slow_frame_line = std::chrono::steady_clock::time_point{};
  int slow_frames_skipped = 0;
#if UI_SDL_VERSION_MAJOR >= 3
  // Live layout+Present while the OS modal resize loop blocks Poll/WaitEvent.
  Backend::SetLiveResizeHandler(context, [this](ui::Context* ctx) {
    if (!ctx)
      return;
    Backend::SyncContext(ctx);
    shell_->Update(ctx);
    ctx->Update();
    chat_->AfterLayout();
    shell_->NotifyFrameEnd(ctx);
    if (!Backend::CanRender())
      return;
    Backend::BeginFrame();
    ctx->Render();
    Backend::PresentFrame();
  });
#endif
  while (true) {
    // Belt-and-suspenders: tasks already queued before this iteration (or wake missed).
    // Primary path is SetUIWakeCallback → RequestForceFrame on every PostUI.
    if (AppRuntime::HasPendingUITasks()) {
      Backend::RequestForceFrame();
    }
    if (!Backend::ProcessEvents(context, ProcessKeyDown, true)) {
      break;
    }
    UiCostCounters::Instance().AddLoop();
    // The frame's work starts with the posted UI tasks: that is where a new message's display sync runs.
    const auto frame_started = std::chrono::steady_clock::now();
    const bool count_slow_frame = logged_first_present; // false for the first presented frame (startup)
    if (AppRuntime::HasPendingUITasks()) {
      UiCostCounters::Instance().AddUiTaskTurn(); // RunUITasks does not say how many it ran
    }
    AppRuntime::RunUITasks();
    const auto tasks_done = std::chrono::steady_clock::now();

    if (shell_->State().nav_tab == NavTab::Me) {
      settings_->Tick();
    }
    // The contact page also ticks while it is a page sheet over another tab (its edits, its close).
    if (shell_->State().nav_tab == NavTab::Contacts || contacts_->DetailSheetOpen()) {
      contacts_->Tick();
    }
    {
      UiCostTimer timer(&UiCostCounters::AddCallTickUs);
      call_->Tick();
    }
    qr_scan_->Tick();
    avatar_crop_->Tick();
    {
      UiCostTimer timer(&UiCostCounters::AddChatUpdateUs);
      chat_->Update();
    }
    ContextMenuHost::Instance().Update();
    {
      UiCostTimer timer(&UiCostCounters::AddShellUpdateUs);
      shell_->Update(context);
    }
    {
      UiCostTimer timer(&UiCostCounters::AddCtxUpdateUs);
      context->Update();
    }
    const auto layout_done = std::chrono::steady_clock::now();
    chat_->AfterLayout();
    // After Context::Update (which resets next_update_timeout): arm power-save for shell timers.
    shell_->NotifyFrameEnd(context);
    qr_scan_->NotifyFrameEnd();
    avatar_crop_->NotifyFrameEnd();
    // A phone in the background: events and posted tasks run above, nothing draws (B127).
    const bool draw_frame = ShouldDrawFrame(Platform::IsMobile(), AppLifecycle::IsForeground());
    // Skip Clear/Present when the Android EGL surface is gone or size is not ready yet.
    if (draw_frame && Backend::CanRender()) {
      const auto draw_started = std::chrono::steady_clock::now(); // after AfterLayout / NotifyFrameEnd
      Backend::BeginFrame();
      context->Render();
      // Our own work ends here. Present can block on vsync or an occluded window, which is not jank.
      const auto draw_done = std::chrono::steady_clock::now();
      Backend::PresentFrame();
      if (ui_perf_ && ui_perf_->IsOpen()) {
        ui_perf_->OnFrame(std::chrono::steady_clock::now()); // engine test page: frame-to-frame timing
      }
      if (!logged_first_present) {
        first_present_seen->store(true);
        StartupMark("first_present");
        MetricsLine("app.start")
            .Add("first_present_ms", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              std::chrono::steady_clock::now() - StartupEpoch())
                                                              .count()))
            .Emit();
        logged_first_present = true;
        first_present_ms_.store(MsSinceStartup());
        if (debug_call_tap_delay_ms_ > 0) {
          // The timer fires on the coordinator; the tap is recorded on the UI thread (nothing captured: it only touches
          // the process-wide PendingNotificationTap, and a post after teardown is dropped).
          AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(debug_call_tap_delay_ms_),
                                                 []() { AppRuntime::PostUI([]() { RecordDebugCallTap(); }); });
        }
        if (ui_perf_ && UiPerfTestController::OpenAtStartupRequested()) {
          AppRuntime::PostUI([this]() { ui_perf_->Open(); }); // --ui-perf-test
        }
        AppRuntime::PostUI([this]() {
          OnFirstPresentDeferredStartup(*client_compat_, *unlock_gate_, MakeShellNavigationPorts(*shell_));
        });
      }
      skip_log_countdown = 0;
      // A frame whose own work took long enough to be felt (UI tasks + update/layout + draw; waiting for
      // the swap is left out). The first presented
      // frame is not counted, and at most one line a second is written, carrying how many were skipped.
      const auto frame_done = std::chrono::steady_clock::now();
      UiFrameStats::AddFrame(std::chrono::duration_cast<std::chrono::microseconds>(layout_done - tasks_done).count(),
                             std::chrono::duration_cast<std::chrono::microseconds>(draw_done - draw_started).count());
      const auto frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(draw_done - frame_started).count();
      if (count_slow_frame && frame_ms >= kSlowFrameMs) {
        UiFrameStats::AddSlowFrame();
        if (frame_done - last_slow_frame_line >= std::chrono::seconds(1)) {
          MetricsLine("ui.slow_frame")
              .Add("ms", static_cast<int64_t>(frame_ms))
              .Add("tasks_ms",
                   static_cast<int64_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(tasks_done - frame_started).count()))
              .Add("layout_ms",
                   static_cast<int64_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(layout_done - tasks_done).count()))
              .Add("draw_ms",
                   static_cast<int64_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(draw_done - draw_started).count()))
              .Add("skipped", static_cast<int64_t>(slow_frames_skipped))
              .Emit();
          last_slow_frame_line = frame_done;
          slow_frames_skipped = 0;
        } else {
          ++slow_frames_skipped;
        }
      }
    } else if (draw_frame && skip_log_countdown-- <= 0) {
      log().warning << "CanRender=false; skipping frame (docs=" << context->GetNumDocuments() << ")";
      skip_log_countdown = 120;
    }
  }
#if UI_SDL_VERSION_MAJOR >= 3
  Backend::SetLiveResizeHandler(nullptr, nullptr);
#endif
}

void Application::Shutdown() {
  StartupMark("shutdown_begin");
  StartupPhase shutdown_total("Application::Shutdown");
  StopGraceTimer();
  AppLifecycle::RemoveListener(grace_foreground_listener_);
  AppLifecycle::RemoveListener(grace_background_listener_);
  AppLifecycle::RemoveListener(attentive_listener_);
  AppLifecycle::RemoveListener(remote_notification_listener_);
  remote_notification_listener_ = 0;
  grace_foreground_listener_ = 0;
  grace_background_listener_ = 0;
  attentive_listener_ = 0;
  AppRuntime::BeginShutdown();
  // Idempotent with Backend::RequestExit — ensures window is gone before joins even if
  // Shutdown is invoked without going through RequestExit (failed Initialize, tests).
  Backend::HideWindow();
  StartupMark("shutdown_window_hidden");
  // Arm the product's exit deadline before joining an OS camera open (which may be in a driver).
  if (avatar_view_) {
    avatar_view_->Shutdown();
    avatar_view_->BindShellNavigation({});
    avatar_view_->BindFlowCoordinator({});
  }
  if (avatar_crop_) {
    avatar_crop_->Shutdown();
    avatar_crop_->BindShellNavigation({});
    avatar_crop_->BindFlowCoordinator({});
  }
  if (qr_scan_) {
    qr_scan_->Shutdown();
    qr_scan_->BindShellNavigation({});
    qr_scan_->BindFlowCoordinator({});
    qr_scan_->BindShowMyQr({});
  }
  contacts_->BindScanPorts({});
  contacts_->BindAccountPorts({});
  contacts_->BindCallActions({});
  contacts_->BindStrangerBlocker(nullptr);
  contacts_->BindStrangerAdder(nullptr);
  shell_->BindScanAction({});

  // Cheap context flags for dogfood latency diagnosis (grep [startup] shutdown_context).
  {
    const bool call_active = product_ != nullptr && product_->Calls().Stack().HasActiveLocalCall();
    const bool connect_inflight = product_ != nullptr && product_->Calls().Stack().IsConnectWorkerInflight();
    const size_t worker_queued = AppRuntime::IsRunning() ? AppRuntime::WorkerTotalQueuedCount() : 0;
    StartupLog().info << "[startup] shutdown_context call_active=" << (call_active ? 1 : 0)
                      << " connect_inflight=" << (connect_inflight ? 1 : 0) << " worker_queued=" << worker_queued
                      << " shutdown_gen=" << AppRuntime::ShutdownGeneration();
  }
  settings_->BindCommands({});
  settings_->BindShellNavigation({});
  settings_->BindShellFeedback({});
  contacts_->BindShellNavigation({});
  contacts_->BindShellFeedback({});
  contacts_->BindSurfaceNotify({});
  if (contacts_shell_bridge_) {
    contacts_shell_bridge_->Clear();
  }
  contacts_->BindContactsPorts({});
  chat_->BindShellNavigation({});
  chat_->BindShellFeedback({});
  chat_->BindSurfaceNotify({});
  if (chat_shell_bridge_) {
    chat_shell_bridge_->Clear();
  }
  chat_->BindShellSetup({});
  chat_->BindConversationsFacade(nullptr);
  chat_->BindAgentPorts({});
  if (messaging_facade_) {
    messaging_facade_->BindAgentInbound({});
  }
  chat_->BindContactsNotify({});
  people_picker_->BindContactsNotify({});
  chat_->BindPeoplePickerNotify({});
  chat_->BindEmojiPickerNotify({});
  chat_->BindMessagingUi({});
  UserFeedback::BindPorts({});
  ShellFeedback::BindChromePorts({});
  contacts_->BindChatPorts({});
  people_picker_->BindChatPorts({});
  people_picker_->BindShellNavigation({});
  people_picker_->BindShellFeedback({});
  people_picker_->BindSurfaceNotify({});
  if (people_picker_shell_bridge_) {
    people_picker_shell_bridge_->Clear();
  }
  if (emoji_picker_) {
    emoji_picker_->BindShellNavigation({});
    emoji_picker_->BindShellFeedback({});
    emoji_picker_->BindFlowCoordinator({});
  }
  people_picker_->BindContactsPorts({});
  people_picker_->BindPickerPorts({});
  if (client_compat_) {
    client_compat_->BindCompatPorts({});
    client_compat_->BindShellFeedback({});
    client_compat_->BindSupportDiscovery({});
  }
  if (flow_) {
    flow_->BindShellNavigation({});
  }
  if (badges_) {
    badges_->BindSource({});
    badges_->BindShellNavigation({});
  }
  if (pin_gate_) {
    pin_gate_->BindShellPinGate({});
    pin_gate_->BindGateComplete({});
  }
  if (call_) {
    call_->BindCallPorts({});
    call_->BindShellCallChrome({});
    call_->BindPeerLinkRefresh(nullptr);
    call_->BindPeoplePickerNotify({});
    call_->BindPinLocked({});
  }
  if (chat_) {
    chat_->BindCallActions({});
    chat_->BindBadgeNotify({});
    chat_->BindUnlockEnsure({});
  }
  if (shell_) {
    shell_->BindCallActions({});
    shell_->BindPinGateActions({});
    shell_->BindFlowCoordinator({});
  }
  if (people_picker_) {
    people_picker_->BindCallActions({});
    people_picker_->BindUnlockEnsure({});
    people_picker_->BindFlowCoordinator({});
  }
  if (settings_) {
    settings_->BindUnlockEnsure({});
  }
  if (contacts_) {
    contacts_->BindUnlockEnsure({});
  }
  IncomingLink::SetHandler({});
  call_ui_.reset();
  shell_->BindShellMessaging({});
  PeoplePickerController::ClearInstance();
  ContactsController::ClearInstance();
  g_input_coordinator = nullptr;
  if (unlock_gate_) {
    unlock_gate_->BindPorts({});
  }

  if (messaging_) {
    messaging_->SetOnMessagingReady(nullptr);
    messaging_->SetOnMeshReady(nullptr);
    messaging_->SetOnReachabilityUpdated(nullptr);
    messaging_->SetOnLinkStateChanged(nullptr);
    messaging_->SetOnPeerIconsChanged(nullptr);
    messaging_->SetOnCallWake(nullptr);
  }

  // Join notification watcher first so it cannot PostTask during teardown, and
  // so process exit does not std::terminate on an unjoined std::thread.
  {
    StartupPhase phase("Shutdown::LocalNotifier");
    ILocalNotifier::Instance().Shutdown();
  }

  if (initialized_) {
#if UI_SDL_VERSION_MAJOR >= 3
    Backend::SetLiveResizeHandler(nullptr, nullptr);
#endif
    {
      StartupPhase phase("Shutdown::ChatController");
      chat_->Shutdown();
    }
    agent_sessions_.reset();

    // Join ringtone before AbortCallMedia / SDL_Quit — accept-dialog quit left the playback
    // worker holding the device (async Stop is Accept-safe and does not join).
    if (call_) {
      StartupPhase phase("Shutdown::CallRingtone");
      call_->PrepareForShutdown();
    }

    // RequestShutdown first so an in-flight EnsureMessagingReady does not finish StartMesh during
    // join. It already AbortCallMediaForShutdown (PrepareForTeardown is non-blocking). Then
    // StopMesh via ShutdownMessaging joins MeshPump while AppRuntime is up.
    if (messaging_) {
      StartupPhase phase("Shutdown::RequestShutdown");
      messaging_->RequestShutdown();
      StartupMark("shutdown_abort_call_media_done");
    }
    // Owners post raw `this` to workers / coordinator / UI; let queued work (e.g. call_leave)
    // finish and close the runtime gate before freeing them (THREADING.md § Teardown quiesce).
    const bool quiesced = [&] {
      StartupPhase phase("Shutdown::QuiesceRuntime");
      return AppRuntime::QuiesceForTeardown(kQuitQuiesceBudget);
    }();
    if (quiesced) {
      StartupPhase phase("Shutdown::MessagingAndMesh");
      ShutdownMessaging();
    } else if (messaging_) {
      // Work still running may touch messaging: persist, then keep it alive until exit.
      log().warning << "Shutdown: runtime work still running — skipping messaging teardown";
      messaging_->FlushForExit();
      (void)product_.release();
    }
    StartupMark("shutdown_stop_mesh_done");
    if (AppRuntime::IsRunning()) {
      StartupPhase phase("Shutdown::AppRuntime");
      AppRuntime::Shutdown();
    }
    StartupMark("shutdown_runtime_join_done");

    AppRuntime::RunUITasks();

    {
      StartupPhase phase("Shutdown::RmlUi");
      ui::RemoveContext("main");
#ifdef PPBROWSER_ENABLE_DEBUGGER
      ui::Debugger::Shutdown();
#endif
      ui::Shutdown();
      harfbuzz_font_engine_.reset();
    }
    AppRuntime::SetUIWakeCallback(nullptr);
    {
      // Released call-media / ringtone leases close on the device thread — finish before SDL_Quit.
      StartupPhase phase("Shutdown::MediaDevices");
      if (!MediaDeviceArbiter::ShutdownDefault(kQuitMediaDevicesBudget)) {
        log().warning << "Shutdown: media device closes still running — detached";
      }
    }
    {
      StartupPhase phase("Shutdown::Backend");
      Backend::Shutdown();
    }
    StartupMark("shutdown_backend_quit_done");

    log().info << "Shutdown complete";
    StartupMark("shutdown_complete");
    initialized_ = false;
  } else {
    // Initialize may have failed after Bootstrap left hub/secrets open.
    if (call_) {
      call_->PrepareForShutdown();
    }
    if (messaging_) {
      messaging_->RequestShutdown();
      messaging_->AbortParticipantsForShutdown();
    }
    ShutdownMessaging();
    if (AppRuntime::IsRunning()) {
      StartupPhase phase("Shutdown::AppRuntime");
      AppRuntime::Shutdown();
    }
  }

  // Always tear down runners — Initialize may have started them before failing.
  {
    StartupPhase phase("Shutdown::AppRuntimeUI");
    AppRuntime::ShutdownUI();
  }
  if (AppRuntime::IsRunning()) {
    StartupPhase phase("Shutdown::AppRuntime");
    AppRuntime::Shutdown();
  }
}

} // namespace pbr
