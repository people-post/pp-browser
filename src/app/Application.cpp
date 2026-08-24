#include "app/Application.h"
#include "app/ConfigApplyBridge.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/crypto/ProfileUnlockGate.h"
#include "base/data/AppPaths.h"
#include "base/data/SchemaVersion.h"
#include "base/data/SessionStore.h"
#include "base/error/AppError.h"
#include "common/Error.h"
#include "base/i18n/LocalizationService.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/MessagingLimits.h"
#include "base/runtime/ProductBranding.h"
#include "base/ui/InputCoordinator.h"
#include "base/ui/ContextMenuHost.h"
#include "base/ui/ViewCatalog.h"
#include "feature/ai/bindings/ActionRouter.h"
#include "feature/chat/ChatController.h"
#include "feature/chat/MessagingTools.h"
#include "feature/settings/SettingsTools.h"
#include "base/runtime/AppRuntime.h"
#include "base/platform/IAssetLocator.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPathProvider.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformServices.h"
#include "base/platform/AppEventHooks.h"
#include "base/platform/MobileWindowSizing.h"
#include "base/platform/NativeFileDialog.h"
#include "base/platform/SdlAppEvents.h"
#include "base/platform/WindowIcon.h"
#include "feature/messaging/AgentUiPorts.h"
#include "feature/messaging/CallFunctionalPorts.h"
#include "feature/messaging/CallUiBackend.h"
#include "feature/messaging/MessagingFacade.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/settings/ReachabilityNudge.h"
#include "feature/settings/SettingsCommands.h"
#include "feature/ui/BadgeNotifyPorts.h"
#include "feature/ui/CallActionsPorts.h"
#include "feature/ui/ChatSessionPorts.h"
#include "app/ChatShellBridge.h"
#include "app/ContactsShellBridge.h"
#include "app/PeoplePickerShellBridge.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/ContactsNotifyPorts.h"
#include "feature/ui/ChatSurfaceNotifyPorts.h"
#include "feature/ui/FlowCoordinatorPorts.h"
#include "feature/ui/PeoplePickerSurfaceNotifyPorts.h"
#include "feature/ui/PinGateActionPorts.h"
#include "feature/ui/ShellChromeApplyPorts.h"
#include "feature/ui/CallController.h"
#include "feature/ui/BadgeAggregator.h"
#include "feature/ui/ClientCompatController.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DeferredStartup.h"
#include "feature/ui/FlowCoordinator.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/UnlockEnsurePorts.h"
#include "feature/ui/UnlockGateCompletePorts.h"
#include "feature/ui/PeoplePickerController.h"
#include "feature/ui/PeoplePickerNotifyPorts.h"
#include "feature/ui/EmojiPickerController.h"
#include "feature/ui/EmojiPickerNotifyPorts.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/ShellSetupPorts.h"
#include "feature/ui/ShellNavigationPorts.h"
#include "feature/ui/UserFeedback.h"
#include "feature/messaging/MessagingCompatPorts.h"
#include "feature/messaging/MessagingContactsPorts.h"
#include "feature/messaging/MessagingShellPorts.h"
#include "feature/messaging/MessagingPeoplePickerPorts.h"
#include "feature/messaging/MessagingUiPorts.h"
#include "feature/ui/ShellCallChromePorts.h"
#include "feature/ui/ShellPinGatePorts.h"
#include "ElementCallVideoTile.h"
#include "base/ui/Theme.h"
#include "common/StartupTiming.h"
#include "base/p2p/Reachability.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/TextLoupe.h>

#include "RmlUi_Backend.h"
#include "RmlUi_Renderer_GL3.h"
#include "TextLoupeRenderer.h"
#include "FontEngineInterfaceHarfBuzz.h"

#ifdef PPBROWSER_ENABLE_DEBUGGER
#include <RmlUi/Debugger.h>
#endif

#include <algorithm>
#include <filesystem>

namespace pbr {

namespace {

InputCoordinator* g_input_coordinator = nullptr;

void WireShellPresentationEvents(ShellHost& shell, BadgeAggregator* badges, SettingsController& settings,
                                 ContactsController& contacts, ChatController& chat) {
  shell.SetOnNavTabChanged([badges, &shell, &settings, &contacts, &chat](NavTab tab) {
    static NavTab previous = NavTab::Home;
    if (previous == NavTab::Me && tab != NavTab::Me) {
      settings.OnMeSurfaceClosed();
    }
    if (tab == NavTab::Home) {
      chat.OnHomeTabActivated();
    }
    if (tab == NavTab::Sessions) {
      chat.OnSessionsTabActivated();
    }
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

  shell.SetOnTransientPopped([&settings, &contacts](const std::string& key) {
    if (key == "contact_detail") {
      contacts.OnDetailDismissed();
    }
    if (key == "settings_detail") {
      settings.OnDetailDismissed();
    }
  });

  shell.SetOnAccountSheetOpened([&settings]() { settings.OnAccountSheetOpened(); });
  shell.SetOnAccountSheetClosed([&settings]() { settings.OnAccountSheetClosed(); });
}

bool ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier,
                    float /*native_dp_ratio*/, bool priority) {
  if (!g_input_coordinator) {
    return true;
  }
  return g_input_coordinator->ProcessKeyDown(context, key, key_modifier, priority);
}

void ApplyUiDocumentLanguage(Rml::Context* context) {
  if (!context || context->GetNumDocuments() == 0) {
    return;
  }
  Rml::ElementDocument* document = context->GetDocument(0);
  if (!document) {
    return;
  }
  document->SetAttribute("lang", LocalizationService::Instance().ResolvedLanguage().c_str());
}

} // namespace

Application::Application() {
  redirectLogger("Application");
  AppRuntime::Initialize();
  secrets_ = std::make_unique<ProfileSecretsService>();
  messaging_ = std::make_unique<MessagingHub>();
  messaging_->BindSessionStore(store_);
  messaging_->BindSecrets(*secrets_);
  messaging_facade_ = std::make_unique<MessagingFacade>(*messaging_);
  config_apply_ = std::make_unique<ConfigApplyBridge>();
  action_router_ = std::make_unique<ActionRouter>();
  client_compat_ = std::make_unique<ClientCompatController>();
  badges_ = std::make_unique<BadgeAggregator>();
  input_ = std::make_unique<InputCoordinator>();
  flow_ = std::make_unique<FlowCoordinator>();
  shell_ = std::make_unique<ShellHost>();
  ShellHost::InstallInstance(*shell_);
  call_ = std::make_unique<CallController>();
  call_ui_ = std::make_unique<CallUiBackend>(messaging_->CallStackRef());
  settings_ = std::make_unique<SettingsController>();
  SettingsController::InstallInstance(*settings_);
  contacts_ = std::make_unique<ContactsController>();
  ContactsController::InstallInstance(*contacts_);
  contacts_shell_bridge_ = std::make_unique<ContactsShellBridge>();
  chat_shell_bridge_ = std::make_unique<ChatShellBridge>();
  people_picker_shell_bridge_ = std::make_unique<PeoplePickerShellBridge>();
  people_picker_ = std::make_unique<PeoplePickerController>();
  PeoplePickerController::InstallInstance(*people_picker_);
  emoji_picker_ = std::make_unique<EmojiPickerController>();
  EmojiPickerController::InstallInstance(*emoji_picker_);
  chat_ = std::make_unique<ChatController>();
  ChatController::InstallInstance(*chat_);
  unlock_gate_ = std::make_unique<ProfileUnlockGate>();
  pin_gate_ = std::make_unique<PinGateController>();
  g_input_coordinator = input_.get();
}

Application::~Application() {
  EmojiPickerController::ClearInstance();
  PeoplePickerController::ClearInstance();
  ContactsController::ClearInstance();
  SettingsController::ClearInstance();
  Shutdown();
  messaging_facade_.reset();
  messaging_.reset();
}

MessagingHub& Application::Messaging() {
  return *messaging_;
}

ProfileSecretsService& Application::Secrets() {
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
    StartupPhase phase("Shutdown::MessagingHub");
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

  ShutdownMessaging();

  std::error_code ec;
  std::filesystem::remove_all(profile_dir, ec);
  if (ec) {
    log().error << "remove_all(" << profile_dir << "): " << ec.message();
    return AppError::Storage(Err::Storage::Failed, "Failed to delete profile data: " + ec.message());
  }

  AppPaths::EnsureDirs(profile_dir);
  if (auto manifest = SchemaVersion::EnsureProfileManifest(profile_dir); !manifest) {
    return manifest.error();
  }

  if (auto secrets = secrets_->Initialize(profile_dir); !secrets) {
    return secrets.error();
  }

  if (auto hub = messaging_->Initialize(config, profile_dir); !hub) {
    return hub.error();
  }

  if (auto prefs = store_.ReloadProfilePrefs(); !prefs) {
    return prefs.error();
  }

  chat_->OnProfileDataReset();
  contacts_->Refresh();
  return {};
}

std::string Application::AssetsPath(const std::string& relative) {
  return IAssetLocator::Instance().Resolve(relative);
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

  AppRuntime::InitializeUI();

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

#if RMLUI_SDL_VERSION_MAJOR >= 3
  if (!Platform::IsMobile()) {
    if (auto* window = Backend::GetWindow()) {
      if (!SetWindowIconFromAsset(window, kAppIconAsset)) {
        log().warning << "Failed to load window icon from " << kAppIconAsset;
      }
    }
  }
#endif

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());

  if (Rml::FileInterface* packaged_files = PlatformServices::PackagedFileInterface()) {
    Rml::SetFileInterface(packaged_files);
  }

  harfbuzz_font_engine_ = std::make_unique<FontEngineInterfaceHarfBuzz>();
  harfbuzz_font_engine_->RegisterLanguage("en", "Latn", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("zh-Hans", "Hans", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("zh-Hant", "Hant", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("ja", "Jpan", TextFlowDirection::LeftToRight);
  harfbuzz_font_engine_->RegisterLanguage("ko", "Kore", TextFlowDirection::LeftToRight);
  Rml::SetFontEngineInterface(harfbuzz_font_engine_.get());

  if (![&] {
        StartupPhase phase("Rml::Initialise");
        return Rml::Initialise();
      }()) {
    log().error << "Rml::Initialise failed";
    Backend::Shutdown();
    return false;
  }
  RegisterCallVideoTileElement();

#ifdef PPBROWSER_ENABLE_DEBUGGER
  Rml::Debugger::Initialise(Rml::CreateContext("debugger", Rml::Vector2i(0, 0)));
#endif

  const std::string theme_path = AssetsPath(bootstrap.profile_prefs.theme);
  Theme::LoadBase(theme_path);
  {
    StartupPhase phase("LoadFontFace:LatoLatin");
    Rml::LoadFontFace(AssetsPath("fonts/LatoLatin-Regular.ttf"));
  }

  {
    StartupPhase phase("Localization::LoadFromAssets");
    if (auto loaded = LocalizationService::Instance().LoadFromAssets(IPathProvider::Instance().BundleAssetsDir());
        !loaded) {
      log().warning << "Localization catalogs failed to load: " << loaded.error().message;
    }
    LocalizationService::Instance().SetPreferredLanguage(bootstrap.profile_prefs.language);
  }

  auto* context = Rml::CreateContext("main", Rml::Vector2i(window_width, window_height));
  if (!context) {
    log().error << "Rml::CreateContext failed";
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

  context->SetTextLoupeRenderCallback([context](Rml::TextLoupePhase phase, const Rml::TextLoupeState& state, Rml::RenderManager&) {
    TextLoupeRenderer::Render(phase, state, static_cast<RenderInterface_GL3&>(*Backend::GetRenderInterface()),
      context->GetDensityIndependentPixelRatio());
  });

  Backend::SyncContext(context);

  const AppearanceMode appearance =
      Theme::ParseAppearance(bootstrap.profile_prefs.appearance);
  Theme::ApplyAppearance(context, appearance);

  SetAppEventHooks(AppEventHooks{
      .on_sync_system_theme =
          [](Rml::Context* ctx) {
            Theme::SyncSystemTheme(ctx);
          },
      .on_context_pointer =
          [](Rml::Context* ctx, int x, int y) {
            return ContextMenuHost::Instance().OnContextPointer(ctx, x, y);
          },
  });
  SdlAppEvents::Install();

  ContextMenuHost::Instance().Install(context);

  action_router_->Attach(context);
  action_router_->SetModelDirtyCallback([](const std::string& model, const std::string& binding) {
    DataModelHost::Instance().Dirty(model, binding);
  });
  MessagingHub& messaging = Messaging();
  MessagingFacade& facade = *messaging_facade_;
  SettingsCommands settings_commands;
  settings_commands.load_profile_identity = [&facade]() {
    return facade.LoadProfileIdentityView();
  };
  settings_commands.save_profile_nickname = [&facade](const std::string& nickname) {
    return facade.SaveProfileNickname(nickname);
  };
  settings_commands.pick_profile_icon_image = [](std::function<void(std::vector<std::string> paths)> on_picked) {
    ShowOpenImageFileDialog(Backend::GetWindow(), [on_picked = std::move(on_picked)](std::vector<std::string> paths) {
      AppRuntime::PostUI([on_picked = std::move(on_picked), paths = std::move(paths)]() mutable {
        on_picked(std::move(paths));
      });
    });
  };
  settings_commands.upload_profile_icon_file = [&facade](const std::string& path) {
    return facade.UploadProfileIconFromPath(path);
  };
  settings_commands.clear_profile_icon = [&facade]() { return facade.ClearProfileIcon(); };
  settings_commands.plan_relay_quota_recovery = [&facade]() { return facade.PlanRelayQuotaRecovery(); };
  settings_commands.free_oldest_relay_blob_slot = [&facade]() { return facade.FreeOldestRelayBlobSlot(); };
  settings_commands.drain_pending_attachment_media = [&facade]() { facade.DrainPendingAttachmentMedia(); };
  settings_commands.register_identity = [this, &facade](const RegisterIdentityArgs& args) {
    auto result = facade.RegisterIdentity(args.nickname);
    if (result) {
      chat_->ReloadAgentConfig();
    }
    return result;
  };
  settings_commands.rotate_brief_llm_key = [this, &facade]() {
    auto result = facade.RotateBriefLlmKey();
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
  settings_commands.run_reachability_probe = [&facade](bool try_upnp) {
    facade.RunReachabilityProbe(try_upnp);
  };
  settings_commands.try_upnp_port_mapping = [&facade]() { facade.TryUpnpPortMapping(); };
  settings_commands.reset_active_profile = [this]() { return ResetActiveProfile(); };
  settings_commands.language_display_label = [](const std::string& pref) {
    return LocalizationService::Instance().LanguageDisplayLabel(pref);
  };
  settings_commands.available_locales = []() {
    return LocalizationService::Instance().AvailableLocales();
  };
  settings_commands.apply_appearance = [](const std::string& appearance_pref) {
    // Settings tools run on the worker pool; RmlUi theme activation is UI-thread only.
    AppRuntime::PostUI([appearance_pref]() {
      if (auto* ctx = Rml::GetContext("main")) {
        Theme::ApplyAppearance(ctx, Theme::ParseAppearance(appearance_pref));
      }
    });
  };
  settings_commands.session_store = [this]() -> SessionStore& { return store_; };
  settings_commands.reload_from_disk = [this]() { return store_.ReloadFromDisk(); };
  settings_commands.messaging_ready = [&facade]() {
    return facade.IsInitialized() && facade.IsMessagingReady();
  };
  settings_commands.last_libp2p_error = [&facade]() -> std::string {
    if (!facade.IsInitialized()) {
      return {};
    }
    return facade.LastLibp2pError();
  };
  settings_commands.load_reachability = [&facade]() {
    SettingsReachabilityView view;
    if (!facade.IsInitialized()) {
      return view;
    }
    const ReachabilitySnapshot snap = facade.Reachability();
    switch (snap.status) {
    case ReachabilityStatus::Checking:
      view.status = SettingsReachabilityView::Status::Checking;
      break;
    case ReachabilityStatus::Reachable:
      view.status = SettingsReachabilityView::Status::Reachable;
      break;
    case ReachabilityStatus::OutboundOnly:
      view.status = SettingsReachabilityView::Status::OutboundOnly;
      break;
    case ReachabilityStatus::Blocked:
      view.status = SettingsReachabilityView::Status::Blocked;
      break;
    case ReachabilityStatus::Unknown:
    default:
      view.status = SettingsReachabilityView::Status::Unknown;
      break;
    }
    view.has_global_ipv6 = snap.signals.has_global_ipv6;
    view.dial_back_ok = snap.signals.dial_back_ok;
    view.upnp_mapped = snap.signals.upnp_mapped;
    view.help_kind = ReachabilityHelpKey(snap.status);
    return view;
  };
  settings_commands.refresh_nav_badges = [this]() {
    if (badges_) {
      badges_->Refresh();
    }
  };
  settings_commands.load_pin_protection = [this]() {
    PinProtectionView view;
    ProfileSecretsService& secrets = *secrets_;
    view.ready = secrets.IsInitialized() && secrets.HasVault();
    view.unlocked = view.ready && secrets.IsUnlocked();
    return view;
  };
  settings_commands.change_pin = [this](const std::string& current_pin,
                                        const std::string& new_pin) -> Roe<void> {
    DataKeyVault* vault = secrets_->Vault();
    if (vault == nullptr) {
      return AppError::Pin(Err::Pin::VaultUnavailable, "Vault unavailable");
    }
    return vault->ChangePin(current_pin, new_pin);
  };
  settings_commands.export_link_device = [&facade]() -> Roe<std::string> { return facade.ExportLinkDevice(); };
  // Copy ports before BindCommands moves the command bag.
  const SettingsToolPorts settings_tool_ports = SettingsToolPortsFromCommands(settings_commands);
  settings_->BindCommands(std::move(settings_commands));
  // SettingsController is constructed before locale catalogs load; rebuild Tr()-backed
  // preference row titles/subtitles (and other localized labels) now that catalogs exist.
  settings_->RefreshLocalizedChrome();

  ShellHost& shell = *shell_;
  const ShellNavigationPorts shell_navigation = MakeShellNavigationPorts(shell);
  const ShellFeedbackPorts shared_feedback = BindSharedShellFeedback(shell);
  settings_->BindShellFeedback(shared_feedback);
  settings_->BindShellNavigation(shell_navigation);

  chat_->BindSessionStore(store_);

  contacts_->BindShellNavigation(shell_navigation);
  contacts_->BindShellFeedback(shared_feedback);
  contacts_shell_bridge_->BindApply(MakeShellChromeApplyPorts(shell, "contacts_chrome"));
  contacts_->BindSurfaceNotify(ContactsSurfaceNotifyPorts{
      .push_surface = [this](const ContactsSurfaceSnapshot& snap) {
        contacts_shell_bridge_->OnSurface(snap);
      },
  });

  chat_->BindShellNavigation(shell_navigation);
  chat_->BindShellFeedback(shared_feedback);
  chat_shell_bridge_->BindApply(MakeShellChromeApplyPorts(shell, "chat_chrome"));
  chat_->BindSurfaceNotify(ChatSurfaceNotifyPorts{
      .push_surface = [this](const ChatSurfaceSnapshot& snap) { chat_shell_bridge_->OnSurface(snap); },
  });
  chat_->BindShellSetup(MakeShellSetupPorts(shell));
  chat_->BindMessagingFacade(messaging_facade_.get());
  chat_->BindRegisterMessagingTools([this, settings_tool_ports](ToolRegistry& tools) {
    RegisterMessagingTools(tools, *messaging_facade_);
    RegisterSettingsTools(tools, settings_tool_ports);
  });
  MessagingUiPorts messaging_ui;
  messaging_ui.snapshot = [&messaging]() { return ProjectMessagingView(messaging); };
  chat_->BindMessagingUi(std::move(messaging_ui));

  contacts_->BindContactsPorts(MakeMessagingContactsPorts(messaging));
  call_->BindCallPorts(
      MakeCallFunctionalPorts(*call_ui_, messaging, store_.IsInitialized() ? &store_ : nullptr));
  call_->BindShellCallChrome(MakeShellCallChromePorts(shell));
  pin_gate_->BindShellPinGate(MakeShellPinGatePorts(shell));
  flow_->BindShellNavigation(shell_navigation);
  people_picker_->BindContactsPorts(MakeMessagingContactsPorts(messaging));
  people_picker_->BindPickerPorts(MakeMessagingPeoplePickerPorts(messaging));
  people_picker_->BindShellNavigation(shell_navigation);
  people_picker_->BindShellFeedback(shared_feedback);
  people_picker_shell_bridge_->BindApply(MakeShellChromeApplyPorts(shell, "people_picker_chrome"));
  people_picker_->BindSurfaceNotify(PeoplePickerSurfaceNotifyPorts{
      .push_surface = [this](const PeoplePickerSurfaceSnapshot& snap) {
        people_picker_shell_bridge_->OnSurface(snap);
      },
  });
  emoji_picker_->BindShellNavigation(shell_navigation);
  emoji_picker_->BindShellFeedback(shared_feedback);
  emoji_picker_->BindSessionStore(store_);
  client_compat_->BindCompatPorts(MakeMessagingCompatPorts(messaging));
  client_compat_->BindShellFeedback(shared_feedback);
  badges_->BindShellNavigation(shell_navigation);
  badges_->BindSource([this, &facade, &shell]() {
    BadgeUnreadInputs inputs;
    if (!facade.IsInitialized()) {
      return inputs;
    }
    const int total = facade.SumUnread();
    int deduction = 0;
    if (shell.State().nav_tab == NavTab::Sessions) {
      const std::string& active_id = facade.ActiveThreadId();
      if (!active_id.empty()) {
        auto thread = facade.GetThread(active_id);
        if (thread && *thread) {
          deduction = (*thread)->unread_count;
        }
      }
    }
    inputs.sessions_unread = std::max(0, total - deduction);
    if (contacts_shell_bridge_) {
      inputs.contacts_unread = std::max(0, contacts_shell_bridge_->LastSurface().contacts_unread);
    }
    if (Platform::IsDesktop() && store_.Snapshot().config.libp2p.node_enabled) {
      const ReachabilitySnapshot snap = facade.Reachability();
      const std::string status_key = ReachabilityStatusKey(snap.status);
      inputs.me_attention = ReachabilityNudgeActive(
          true, status_key, store_.Snapshot().profile_prefs.reachability_nudge_acked_status);
    }
    return inputs;
  });
  {
    BadgeNotifyPorts badge_notify;
    badge_notify.refresh = [this]() {
      if (badges_) {
        badges_->Refresh();
      }
    };
    badge_notify.sessions_unread = [this]() {
      return badges_ ? badges_->State().sessions_unread : 0;
    };
    chat_->BindBadgeNotify(std::move(badge_notify));
  }
  chat_->BindInputCoordinator(*input_);
  {
    CallActionsPorts call_actions;
    call_actions.start_call = [this](const std::string& thread_id, const bool video_allowed) {
      return call_->StartCall(thread_id, video_allowed);
    };
    call_actions.refresh_pending_ring = [this]() { call_->RefreshPendingRing(); };
    call_actions.invite_identities = [this](const std::vector<std::string>& identities) {
      call_->InviteIdentitiesToActiveCall(identities);
    };
    call_actions.start_with_invitees =
        [this](const std::string& thread_id, const bool video_allowed,
               const std::vector<std::string>& identities) {
          return call_->StartCallWithInvitees(thread_id, video_allowed, identities);
        };
    call_actions.accept_incoming = [this]() { call_->AcceptIncoming(); };
    call_actions.accept_incoming_with_charge = [this]() { call_->AcceptIncomingWithCharge(); };
    call_actions.decline_incoming = [this]() { call_->DeclineIncoming(); };
    call_actions.leave_active = [this]() { call_->LeaveActive(); };
    call_actions.retry_connect = [this]() { call_->RetryConnect(); };
    call_actions.toggle_mute = [this]() { call_->ToggleMute(); };
    call_actions.toggle_camera = [this]() { call_->ToggleCamera(); };
    call_actions.toggle_speaker = [this]() { call_->ToggleSpeaker(); };
    call_actions.open_mid_call_invite_picker = [this]() { call_->OpenMidCallInvitePicker(); };
    call_actions.minimize_chrome = [this]() { call_->MinimizeChrome(); };
    call_actions.expand_chrome = [this]() { call_->ExpandChrome(); };
    call_actions.immersive_chrome = [this]() { call_->ImmersiveChrome(); };
    call_actions.restore_chrome_from_minimized = [this]() { call_->RestoreChromeFromMinimized(); };
    call_actions.set_minimized_corner = [this](int corner) { call_->SetMinimizedCorner(corner); };
    call_actions.show_call_details = [this]() { call_->ShowCallDetails(); };
    chat_->BindCallActions(call_actions);
    shell_->BindCallActions(call_actions);
    people_picker_->BindCallActions(std::move(call_actions));
  }

  unlock_gate_->BindSecrets(*secrets_);
  {
    UnlockGateCompletePorts gate_complete;
    gate_complete.complete_with_pin = [this](const std::string& pin, const bool create_mode) {
      unlock_gate_->CompleteWithPin(pin, create_mode);
    };
    gate_complete.complete_with_default_pin = [this]() { unlock_gate_->CompleteWithDefaultPin(); };
    gate_complete.complete_link_device = [this](const std::string& pin, const std::string& bundle_json,
                                                const bool set_default_pin) {
      unlock_gate_->CompleteLinkDevice(pin, bundle_json, set_default_pin);
    };
    gate_complete.cancel = [this]() { unlock_gate_->Cancel(); };
    pin_gate_->BindGateComplete(std::move(gate_complete));
  }
  ProfileUnlockPorts unlock_ports;
  unlock_ports.messaging_initialized = [&messaging]() { return messaging.IsInitialized(); };
  unlock_ports.messaging_ready = [&messaging]() { return messaging.IsMessagingReady(); };
  unlock_ports.ensure_messaging_ready = [&messaging]() { return messaging.EnsureMessagingReady(); };
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
  unlock_ports.ui.show_identity_fork = [this]() { pin_gate_->ShowIdentityFork(); };
  unlock_ports.ui.show_chooser = [this]() { pin_gate_->ShowChooser(); };
  unlock_ports.ui.show_unlock = [this]() { pin_gate_->ShowUnlock(); };
  unlock_ports.ui.dismiss = [this]() { pin_gate_->Dismiss(); };
  unlock_ports.ui.set_unlock_in_progress = [this](const bool in_progress) {
    pin_gate_->SetUnlockInProgressUi(in_progress);
  };
  unlock_ports.ui.show_error = [this](const std::string& message) { pin_gate_->ShowError(message); };
  unlock_ports.ui.on_default_provisioned = []() {
    UserFeedback::Ok("Using the app default. Change anytime in Me → Security.");
  };
  unlock_ports.import_link_device = [&facade](const std::string& pin,
                                             const std::string& bundle_json) -> Roe<void> {
    return facade.ImportLinkDevice(bundle_json, pin);
  };
  unlock_ports.ui.on_link_imported = [this, &facade]() {
    // Import runs before EnsureMessagingReady; push attach after the vault is open.
    AppRuntime::PostWorkerNormal([this, &facade]() {
      (void)facade.SyncPushDevices(Store().Snapshot().profile_prefs.show_notifications);
    });
    if (chat_) {
      chat_->ReloadAgentConfig();
    }
    UserFeedback::Ok(Tr("pin.link_imported"));
  };
  // Argon2 + libp2p stack init must not block the UI thread (Android / iOS first unlock).
  unlock_ports.run_heavy = [](std::function<Roe<void>()> work,
                              std::function<void(Roe<void>)> on_done) {
    // Cold-start surface blips can PauseBackgroundWork without Resume; unlock must still run.
    AppRuntime::ResumeBackgroundWork();
    AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(WorkerLane::Normal, std::move(work), std::move(on_done));
  };
  unlock_gate_->BindPorts(std::move(unlock_ports));

  {
    UnlockEnsurePorts unlock_ensure;
    unlock_ensure.ensure_unlocked = [this](std::function<void(bool)> done) {
      unlock_gate_->EnsureUnlocked(std::move(done));
    };
    unlock_ensure.is_unlock_in_progress = [this]() { return unlock_gate_->IsUnlockInProgress(); };
    chat_->BindUnlockEnsure(unlock_ensure);
    settings_->BindUnlockEnsure(unlock_ensure);
    contacts_->BindUnlockEnsure(unlock_ensure);
    people_picker_->BindUnlockEnsure(std::move(unlock_ensure));
  }
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
    shell_->BindPinGateActions(std::move(pin_actions));
  }
  {
    FlowCoordinatorPorts flow_ports;
    flow_ports.begin_modal =
        [this](int layer_id, std::function<bool()> on_step_back, std::function<void()> on_cancel) {
          flow_->BeginModal(layer_id, std::move(on_step_back), std::move(on_cancel));
        };
    flow_ports.end_modal = [this]() { flow_->EndModal(); };
    flow_ports.handle_dismiss = [this]() { return flow_->HandleDismiss(); };
    flow_ports.notify_layer_closing = [this](int layer_id) { flow_->NotifyLayerClosing(layer_id); };
    shell_->BindFlowCoordinator(flow_ports);
    people_picker_->BindFlowCoordinator(flow_ports);
    emoji_picker_->BindFlowCoordinator(std::move(flow_ports));
  }

  config_apply_->Bind(messaging, store_, *shell_, *chat_, [](const std::string& relative) { return AssetsPath(relative); });

  agent_session_.emplace();
  chat_->BindAgentPorts(MakeAgentUiPorts(*agent_session_));
  if (agent_session_) {
    agent_session_->SetToolPermissions(store_.Snapshot().profile_prefs.tool_permissions);
    agent_session_->SetToolPermissionsSaver([this](const ToolPermissionsPrefs& permissions) -> Roe<void> {
      ProfilePreferences prefs = store_.Snapshot().profile_prefs;
      prefs.tool_permissions = permissions;
      prefs.schema_version = ProfilePreferences::kSchemaVersion;
      return store_.SaveProfilePrefs(prefs);
    });
    store_.AddProfilePrefsListener([this](const ProfilePreferences& prefs) {
      if (agent_session_) {
        agent_session_->SetToolPermissions(prefs.tool_permissions);
      }
    });
  }

  if (!settings_->RegisterModel(context)) {
    log().error << "SettingsController RegisterModel failed";
    return false;
  }

  if (!contacts_->RegisterModel(context)) {
    log().error << "ContactsController RegisterModel failed";
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

  if (!shell_->RegisterWindowModel(context)) {
    log().error << "ShellHost RegisterWindowModel failed";
    return false;
  }

  ContactsNotifyPorts contacts_notify;
  contacts_notify.refresh = [this]() { contacts_->Refresh(); };
  contacts_notify.select_contact = [this](const std::string& id) { contacts_->OnSelectContact(id); };
  chat_->BindContactsNotify(std::move(contacts_notify));

  PeoplePickerNotifyPorts chat_people_picker_notify;
  chat_people_picker_notify.open_free = [this]() { people_picker_->OpenFree(); };
  chat_people_picker_notify.open_from_dm = [this](const std::string& id) { people_picker_->OpenFromDm(id); };
  chat_->BindPeoplePickerNotify(std::move(chat_people_picker_notify));

  EmojiPickerNotifyPorts emoji_notify;
  emoji_notify.open_insert = [this]() {
    emoji_picker_->OpenInsert([this](std::string emoji, bool restore_focus) {
      chat_->InsertEmojiIntoDraft(emoji, restore_focus);
    });
  };
  emoji_notify.open_react = [this](const std::string& message_id) {
    emoji_picker_->OpenReact(message_id, [this, message_id](std::string emoji) {
      chat_->ReactWithEmoji(message_id, emoji);
    });
  };
  chat_->BindEmojiPickerNotify(std::move(emoji_notify));

  PeoplePickerNotifyPorts call_people_picker_notify;
  call_people_picker_notify.open_for_group_call = [this](const std::string& thread_id) {
    people_picker_->OpenForGroupCall(thread_id);
  };
  call_people_picker_notify.open_for_call_add_guest = [this](const std::string& call_id) {
    people_picker_->OpenForCallAddGuest(call_id);
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
    chat_->BindShellNavigation({});
    chat_->BindShellFeedback({});
    chat_->BindSurfaceNotify({});
    if (chat_shell_bridge_) {
      chat_shell_bridge_->Clear();
    }
    chat_->BindShellSetup({});
    chat_->BindMessagingFacade(nullptr);
    chat_->BindAgentPorts({});
    chat_->BindContactsNotify({});
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
    if (client_compat_) {
      client_compat_->BindCompatPorts({});
      client_compat_->BindShellFeedback({});
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
      call_->BindPeoplePickerNotify({});
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
    }
    if (contacts_) {
      contacts_->BindUnlockEnsure({});
    }
    call_ui_.reset();
    if (unlock_gate_) {
      unlock_gate_->BindPorts({});
    }
    agent_session_.reset();
    Rml::RemoveContext("main");
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

  ChatSessionPorts chat_ports;
  chat_ports.finalize_thread_display = [this]() { chat_->FinalizeThreadDisplay(); };
  chat_ports.select_thread = [this](const std::string& id) { chat_->OnSelectThread(id); };
  chat_ports.on_find_someone = [this]() { chat_->OnFindSomeone(); };
  contacts_->BindChatPorts(chat_ports);
  people_picker_->BindChatPorts(std::move(chat_ports));

  WireShellPresentationEvents(shell, badges_.get(), *settings_, *contacts_, *chat_);
  shell_->SetOnBottomChromeDismissed([this]() {
    if (emoji_picker_) {
      emoji_picker_->Close();
    }
  });

  // After chat exists: fan-out config/prefs, then own hub lifecycle callbacks (not ChatController).
  config_apply_->InstallListeners();

  messaging.SetOnMessagingReady([this]() {
    chat_->OnMessagingReady();
    contacts_->Refresh();
    const ShellChromeSnapshot chrome = ProjectShellChromeSnapshot(shell_->State());
    if (chrome.account_sheet_open) {
      settings_->OnAccountSheetOpened();
    } else if (chrome.nav_tab == NavTab::Me) {
      settings_->OnNavTabActivated();
    }
  });
  messaging.SetOnCallWake([this]() {
    if (call_) {
      call_->OnCallWake();
    }
  });
  messaging.SetOnReachabilityUpdated([this]() {
    AppRuntime::PostUI([this]() {
      const ReachabilitySnapshot snap = messaging_facade_->Reachability();
      if (snap.status == ReachabilityStatus::Reachable) {
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
  messaging.SetOnPeerIconsChanged([this]() {
    AppRuntime::PostUI([this]() {
      if (contacts_) {
        contacts_->Refresh();
      }
      if (call_) {
        call_->RefreshPendingRing();
      }
    });
  });

  LocalizationService::Instance().AddLanguageChangeListener([this](const std::string& /*resolved*/) {
    settings_->RefreshLocalizedChrome();
    ViewCatalog::ClearCache();
    shell_->RequestSyncLayout(true);
    if (auto* ctx = Rml::GetContext("main")) {
      ApplyUiDocumentLanguage(ctx);
    }
  });

  shell_->SetSafeAreaInsetsFromPrefs(bootstrap.machine_prefs.safe_area.top,
                                    bootstrap.machine_prefs.safe_area.bottom);
  shell_->RefreshSafeAreaInsets(context);
  shell_->SyncChromeMaterialPrefs(bootstrap.profile_prefs.reduce_transparency,
                                  bootstrap.profile_prefs.compact_chrome_frost);

  ApplyUiDocumentLanguage(context);

  log().info << "Initialization complete";
  initialized_ = true;
  return true;
}

void Application::Run() {
  if (!initialized_) {
    return;
  }

  auto* context = Rml::GetContext("main");
  if (!context) {
    return;
  }

  int skip_log_countdown = 0;
  bool logged_first_present = false;
#if RMLUI_SDL_VERSION_MAJOR >= 3
  // Live layout+Present while the OS modal resize loop blocks Poll/WaitEvent.
  Backend::SetLiveResizeHandler(context, [](Rml::Context* ctx) {
    if (!ctx)
      return;
    Backend::SyncContext(ctx);
    ShellHost::Instance().Update(ctx);
    ctx->Update();
    ChatController::Instance().AfterLayout();
    ShellHost::Instance().NotifyFrameEnd(ctx);
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
    AppRuntime::RunUITasks();

    if (shell_->State().account_sheet_open || shell_->State().nav_tab == NavTab::Me) {
      settings_->Tick();
    }
    if (shell_->State().nav_tab == NavTab::Contacts) {
      contacts_->Tick();
    }
    call_->Tick();
    chat_->Update();
    ContextMenuHost::Instance().Update();
    shell_->Update(context);
    context->Update();
    chat_->AfterLayout();
    // After Context::Update (which resets next_update_timeout): arm power-save for shell timers.
    shell_->NotifyFrameEnd(context);
    // Skip Clear/Present when the Android EGL surface is gone or size is not ready yet.
    if (Backend::CanRender()) {
      Backend::BeginFrame();
      context->Render();
      Backend::PresentFrame();
      if (!logged_first_present) {
        StartupMark("first_present");
        logged_first_present = true;
        AppRuntime::PostUI([this]() {
          OnFirstPresentDeferredStartup(*client_compat_, *unlock_gate_, MakeShellNavigationPorts(*shell_));
        });
      }
      skip_log_countdown = 0;
    } else if (skip_log_countdown-- <= 0) {
      log().warning << "CanRender=false; skipping frame (docs=" << context->GetNumDocuments() << ")";
      skip_log_countdown = 120;
    }
  }
#if RMLUI_SDL_VERSION_MAJOR >= 3
  Backend::SetLiveResizeHandler(nullptr, nullptr);
#endif
}

void Application::Shutdown() {
  StartupMark("shutdown_begin");
  StartupPhase shutdown_total("Application::Shutdown");

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
  chat_->BindMessagingFacade(nullptr);
  chat_->BindAgentPorts({});
  chat_->BindContactsNotify({});
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
    call_->BindPeoplePickerNotify({});
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
  call_ui_.reset();
  shell_->BindShellMessaging({});
  ChatController::ClearInstance();
  ShellHost::ClearInstance();
  PeoplePickerController::ClearInstance();
  ContactsController::ClearInstance();
  SettingsController::ClearInstance();
  g_input_coordinator = nullptr;
  if (unlock_gate_) {
    unlock_gate_->BindPorts({});
  }

  if (messaging_) {
    messaging_->SetOnMessagingReady(nullptr);
    messaging_->SetOnReachabilityUpdated(nullptr);
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
#if RMLUI_SDL_VERSION_MAJOR >= 3
    Backend::SetLiveResizeHandler(nullptr, nullptr);
#endif
    {
      StartupPhase phase("Shutdown::ChatController");
      chat_->Shutdown();
    }
    agent_session_.reset();

    // Join ringtone before AbortCallMedia / SDL_Quit — accept-dialog quit left the playback
    // worker holding the device (async Stop is Accept-safe and does not join).
    if (call_) {
      StartupPhase phase("Shutdown::CallRingtone");
      call_->PrepareForShutdown();
    }

    // Abort Connect / circuit waits, then join workers while MessagingHub still owns the bridge.
    // Destroying the hub first left AppRuntime::Shutdown joining a UAF Connect worker.
    if (messaging_) {
      StartupPhase phase("Shutdown::AbortCallMedia");
      messaging_->AbortCallMediaForShutdown();
    }
    if (AppRuntime::IsRunning()) {
      StartupPhase phase("Shutdown::AppRuntime");
      AppRuntime::Shutdown();
    }

    ShutdownMessaging();

    AppRuntime::RunUITasks();

    {
      StartupPhase phase("Shutdown::RmlUi");
      action_router_->Detach();
      Rml::RemoveContext("main");
#ifdef PPBROWSER_ENABLE_DEBUGGER
      Rml::Debugger::Shutdown();
#endif
      Rml::Shutdown();
      harfbuzz_font_engine_.reset();
    }
    AppRuntime::SetUIWakeCallback(nullptr);
    {
      StartupPhase phase("Shutdown::Backend");
      Backend::Shutdown();
    }

    log().info << "Shutdown complete";
    initialized_ = false;
  } else {
    // Initialize may have failed after Bootstrap left hub/secrets open.
    if (call_) {
      call_->PrepareForShutdown();
    }
    if (messaging_) {
      messaging_->AbortCallMediaForShutdown();
    }
    if (AppRuntime::IsRunning()) {
      StartupPhase phase("Shutdown::AppRuntime");
      AppRuntime::Shutdown();
    }
    ShutdownMessaging();
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
