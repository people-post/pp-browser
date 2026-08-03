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
#include "base/platform/ProductBranding.h"
#include "base/ui/InputCoordinator.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ai/bindings/ActionRouter.h"
#include "feature/chat/ChatController.h"
#include "feature/chat/MessagingTools.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/PlatformRuntime.h"
#include "base/platform/IAssetLocator.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPathProvider.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformServices.h"
#include "base/platform/AppEventHooks.h"
#include "base/platform/MobileWindowSizing.h"
#include "base/platform/SdlAppEvents.h"
#include "base/platform/WindowIcon.h"
#include "feature/messaging/AgentUiPorts.h"
#include "feature/messaging/MessagingCallPorts.h"
#include "feature/messaging/MessagingChatPorts.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/settings/SettingsCommands.h"
#include "feature/ui/ChatSessionPorts.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/ContactsNotifyPorts.h"
#include "feature/ui/CallController.h"
#include "feature/ui/BadgeAggregator.h"
#include "feature/ui/ClientCompatController.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DeferredStartup.h"
#include "feature/ui/FlowCoordinator.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/PeoplePickerController.h"
#include "feature/ui/PeoplePickerNotifyPorts.h"
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
#include "libp2p/integration/host/Reachability.h"

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

#include <filesystem>
#include <algorithm>

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
    shell.DirtyWindow();
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
  PlatformRuntime::Initialize();
  messaging_ = std::make_unique<MessagingHub>();
  messaging_->BindSessionStore(store_);
  config_apply_ = std::make_unique<ConfigApplyBridge>();
  action_router_ = std::make_unique<ActionRouter>();
  client_compat_ = std::make_unique<ClientCompatController>();
  badges_ = std::make_unique<BadgeAggregator>();
  input_ = std::make_unique<InputCoordinator>();
  flow_ = std::make_unique<FlowCoordinator>();
  shell_ = std::make_unique<ShellHost>();
  ShellHost::InstallInstance(*shell_);
  call_ = std::make_unique<CallController>();
  settings_ = std::make_unique<SettingsController>();
  SettingsController::InstallInstance(*settings_);
  contacts_ = std::make_unique<ContactsController>();
  ContactsController::InstallInstance(*contacts_);
  people_picker_ = std::make_unique<PeoplePickerController>();
  PeoplePickerController::InstallInstance(*people_picker_);
  chat_ = std::make_unique<ChatController>();
  ChatController::InstallInstance(*chat_);
  unlock_gate_ = std::make_unique<ProfileUnlockGate>();
  pin_gate_ = std::make_unique<PinGateController>();
  g_input_coordinator = input_.get();
}

Application::~Application() {
  PeoplePickerController::ClearInstance();
  ContactsController::ClearInstance();
  SettingsController::ClearInstance();
  Shutdown();
  messaging_.reset();
}

MessagingHub& Application::Messaging() {
  return *messaging_;
}

void Application::ShutdownMessaging() {
  if (!messaging_ || !messaging_->IsInitialized()) {
    if (ProfileSecretsService::Instance().IsInitialized()) {
      StartupPhase phase("Shutdown::ProfileSecrets");
      ProfileSecretsService::Instance().Shutdown();
    }
    return;
  }
  {
    StartupPhase phase("Shutdown::MessagingHub");
    messaging_->Shutdown();
  }
  if (ProfileSecretsService::Instance().IsInitialized()) {
    StartupPhase phase("Shutdown::ProfileSecrets");
    ProfileSecretsService::Instance().Shutdown();
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

  if (auto secrets = ProfileSecretsService::Instance().Initialize(profile_dir); !secrets) {
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

  BrowserThread::Initialize();

  if (![&] {
        StartupPhase phase("Backend::Initialize");
        return Backend::Initialize(window_title, window_width, window_height, true, !Platform::IsMobile());
      }()) {
    log().error << "Backend::Initialize failed (SDL/OpenGL window could not be created)";
    return false;
  }
  // Power-save WaitEventTimeout otherwise holds UI posts (toasts, OS notify, inbox) until input.
  BrowserThread::SetUIWakeCallback([]() { Backend::WakeEventLoop(); });

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

  if (Rml::FileInterface* packaged_files = PlatformRuntime::PackagedFileInterface()) {
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
  SettingsCommands settings_commands;
  settings_commands.load_profile_identity = [&messaging]() {
    return messaging.LoadProfileIdentityView();
  };
  settings_commands.save_profile_nickname = [&messaging](const std::string& nickname) {
    return messaging.SaveProfileNickname(nickname);
  };
  settings_commands.register_identity = [this, &messaging](const RegisterIdentityArgs& args) {
    auto result = messaging.RegisterIdentity(args.nickname);
    if (result) {
      chat_->ReloadAgentConfig();
    }
    return result;
  };
  settings_commands.rotate_brief_llm_key = [this, &messaging]() {
    auto result = messaging.RotateBriefLlmKey();
    if (result) {
      chat_->ReloadAgentConfig();
    }
    return result;
  };
  settings_commands.clear_undelivered_older_than = [&messaging](int days) -> Roe<void> {
    if (!messaging.IsInitialized() || !messaging.IsMessagingReady()) {
      return Error("Messaging is not ready");
    }
    if (auto cleared = messaging.P2p().ClearUndeliveredOlderThan(days); !cleared) {
      return cleared.error();
    }
    return {};
  };
  settings_commands.run_reachability_probe = [&messaging](bool try_upnp) {
    messaging.RunReachabilityProbe(try_upnp);
  };
  settings_commands.try_upnp_port_mapping = [&messaging]() { messaging.TryUpnpPortMapping(); };
  settings_commands.reset_active_profile = [this]() { return ResetActiveProfile(); };
  settings_commands.language_display_label = [](const std::string& pref) {
    return LocalizationService::Instance().LanguageDisplayLabel(pref);
  };
  settings_commands.available_locales = []() {
    return LocalizationService::Instance().AvailableLocales();
  };
  settings_commands.apply_appearance = [](const std::string& appearance_pref) {
    if (auto* ctx = Rml::GetContext("main")) {
      Theme::ApplyAppearance(ctx, Theme::ParseAppearance(appearance_pref));
    }
  };
  settings_commands.session_store = [this]() -> SessionStore& { return store_; };
  settings_commands.reload_from_disk = [this]() { return store_.ReloadFromDisk(); };
  settings_commands.messaging_ready = [&messaging]() {
    return messaging.IsInitialized() && messaging.IsMessagingReady();
  };
  settings_commands.last_libp2p_error = [&messaging]() -> std::string {
    if (!messaging.IsInitialized()) {
      return {};
    }
    return messaging.LastLibp2pError();
  };
  settings_commands.load_reachability = [&messaging]() {
    SettingsReachabilityView view;
    if (!messaging.IsInitialized()) {
      return view;
    }
    const ReachabilitySnapshot snap = messaging.Reachability();
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
  settings_commands.load_pin_protection = []() {
    PinProtectionView view;
    auto& secrets = ProfileSecretsService::Instance();
    view.ready = secrets.IsInitialized() && secrets.HasVault();
    view.unlocked = view.ready && secrets.IsUnlocked();
    return view;
  };
  settings_->BindCommands(std::move(settings_commands));

  ShellHost& shell = *shell_;
  const ShellNavigationPorts shell_navigation = MakeShellNavigationPorts(shell);
  const ShellFeedbackPorts shared_feedback = BindSharedShellFeedback(shell);
  settings_->BindShellFeedback(shared_feedback);
  settings_->BindShellNavigation(shell_navigation);

  chat_->BindSessionStore(store_);

  contacts_->BindShellNavigation(shell_navigation);
  contacts_->BindShellFeedback(shared_feedback);

  chat_->BindShellNavigation(shell_navigation);
  chat_->BindShellFeedback(shared_feedback);
  chat_->BindShellSetup(MakeShellSetupPorts(shell));
  MessagingChatPorts messaging_chat_ports = MakeMessagingChatPorts(messaging);
  messaging_chat_ports.register_messaging_tools = [&messaging](ToolRegistry& tools) {
    RegisterMessagingTools(tools, messaging);
  };
  chat_->BindChatPorts(std::move(messaging_chat_ports));
  MessagingUiPorts messaging_ui;
  messaging_ui.snapshot = [&messaging]() { return ProjectMessagingView(messaging); };
  chat_->BindMessagingUi(std::move(messaging_ui));

  contacts_->BindContactsPorts(MakeMessagingContactsPorts(messaging));
  call_->BindCallPorts(MakeMessagingCallPorts(messaging));
  call_->BindShellCallChrome(MakeShellCallChromePorts(shell));
  pin_gate_->BindShellPinGate(MakeShellPinGatePorts(shell));
  flow_->BindShellNavigation(shell_navigation);
  people_picker_->BindContactsPorts(MakeMessagingContactsPorts(messaging));
  people_picker_->BindPickerPorts(MakeMessagingPeoplePickerPorts(messaging));
  people_picker_->BindShellNavigation(shell_navigation);
  people_picker_->BindShellFeedback(shared_feedback);
  client_compat_->BindCompatPorts(MakeMessagingCompatPorts(messaging));
  client_compat_->BindShellFeedback(shared_feedback);
  badges_->BindShellNavigation(shell_navigation);
  badges_->BindSource([&messaging, &shell]() {
    BadgeUnreadInputs inputs;
    if (!messaging.IsInitialized()) {
      return inputs;
    }
    auto& inbox = messaging.Inbox();
    const int total = inbox.SumUnread();
    int deduction = 0;
    if (shell.State().nav_tab == NavTab::Sessions) {
      const std::string& active_id = inbox.ActiveThreadId();
      if (!active_id.empty()) {
        auto thread = messaging.Store().GetThread(active_id);
        if (thread && *thread) {
          deduction = (*thread)->unread_count;
        }
      }
    }
    // Sessions owns aggregate chat unread. Contacts nav stays at 0 until a
    // contacts-tab queue exists (intro requests, pending invites, etc.).
    inputs.sessions_unread = std::max(0, total - deduction);
    inputs.contacts_unread = 0;
    return inputs;
  });
  chat_->BindBadgeAggregator(*badges_);
  chat_->BindInputCoordinator(*input_);
  chat_->BindCallController(*call_);

  unlock_gate_->BindSecrets(ProfileSecretsService::Instance());
  pin_gate_->BindGate(*unlock_gate_);
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
  // Argon2 + libp2p stack init must not block the UI thread (Android / iOS first unlock).
  unlock_ports.run_heavy = [](std::function<Roe<void>()> work,
                              std::function<void(Roe<void>)> on_done) {
    // Cold-start surface blips can PauseIO without Resume; unlock must still run.
    BrowserThread::ResumeIO();
    BrowserThread::PostTaskAndReply<Roe<void>>(std::move(work), std::move(on_done));
  };
  unlock_gate_->BindPorts(std::move(unlock_ports));

  chat_->BindUnlockGate(*unlock_gate_);
  shell_->BindShellMessaging(MakeMessagingShellPorts(messaging));
  shell_->BindPinGate(*pin_gate_);
  shell_->BindFlowCoordinator(*flow_);
  shell_->BindCallController(*call_);
  settings_->BindUnlockGate(*unlock_gate_);
  contacts_->BindUnlockGate(*unlock_gate_);
  people_picker_->BindUnlockGate(*unlock_gate_);
  people_picker_->BindFlowCoordinator(*flow_);
  people_picker_->BindCallController(*call_);

  config_apply_->Bind(messaging, store_, *shell_, *chat_, [](const std::string& relative) { return AssetsPath(relative); });

  agent_session_.emplace();
  chat_->BindAgentPorts(MakeAgentUiPorts(*agent_session_));

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

  PeoplePickerNotifyPorts call_people_picker_notify;
  call_people_picker_notify.open_for_group_call = [this](const std::string& thread_id, bool video) {
    people_picker_->OpenForGroupCall(thread_id, video);
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
    contacts_->BindContactsPorts({});
    chat_->BindShellNavigation({});
    chat_->BindShellFeedback({});
    chat_->BindShellSetup({});
    chat_->BindChatPorts({});
    chat_->BindAgentPorts({});
    chat_->BindContactsNotify({});
    chat_->BindPeoplePickerNotify({});
    chat_->BindMessagingUi({});
    UserFeedback::BindPorts({});
    ShellFeedback::BindChromePorts({});
    contacts_->BindChatPorts({});
    people_picker_->BindChatPorts({});
    people_picker_->BindShellNavigation({});
    people_picker_->BindShellFeedback({});
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
    }
    if (call_) {
      call_->BindCallPorts({});
      call_->BindShellCallChrome({});
      call_->BindPeoplePickerNotify({});
    }
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
  messaging.SetOnReachabilityUpdated([this]() {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
      settings_->SyncReachability();
      const ReachabilitySnapshot snap = Messaging().Reachability();
      if (snap.status == ReachabilityStatus::OutboundOnly || snap.status == ReachabilityStatus::Blocked) {
        UserFeedback::NeedsSetup(Tr("settings.network.banner_hint"));
      }
    });
  });

  LocalizationService::Instance().AddLanguageChangeListener([this](const std::string& /*resolved*/) {
    settings_->RefreshLocalizedChrome();
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
  while (Backend::ProcessEvents(context, ProcessKeyDown, true)) {
    BrowserThread::RunUITasks();
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
        BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
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
  contacts_->BindContactsPorts({});
  chat_->BindShellNavigation({});
  chat_->BindShellFeedback({});
  chat_->BindShellSetup({});
  chat_->BindChatPorts({});
  chat_->BindAgentPorts({});
  chat_->BindContactsNotify({});
  chat_->BindPeoplePickerNotify({});
  chat_->BindMessagingUi({});
  UserFeedback::BindPorts({});
  ShellFeedback::BindChromePorts({});
  contacts_->BindChatPorts({});
  people_picker_->BindChatPorts({});
  people_picker_->BindShellNavigation({});
  people_picker_->BindShellFeedback({});
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
  }
  if (call_) {
    call_->BindCallPorts({});
    call_->BindShellCallChrome({});
    call_->BindPeoplePickerNotify({});
  }
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

    ShutdownMessaging();

    BrowserThread::RunUITasks();

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
    BrowserThread::SetUIWakeCallback(nullptr);
    {
      StartupPhase phase("Shutdown::Backend");
      Backend::Shutdown();
    }

    log().info << "Shutdown complete";
    initialized_ = false;
  } else {
    // Initialize may have failed after Bootstrap left hub/secrets open.
    ShutdownMessaging();
  }

  // Always tear down runners — Initialize may have started them before failing.
  {
    StartupPhase phase("Shutdown::BrowserThread");
    BrowserThread::Shutdown();
  }
  if (PlatformRuntime::IsRunning()) {
    StartupPhase phase("Shutdown::PlatformRuntime");
    PlatformRuntime::Shutdown();
  }
}

} // namespace pbr
