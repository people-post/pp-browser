#include "app/Application.h"

#include "base/ui/InputCoordinator.h"
#include "base/ui/ContextMenuHost.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/MessagingLimits.h"
#include "base/platform/ProductBranding.h"
#include "feature/ai/bindings/ActionRouter.h"
#include "feature/chat/ChatController.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/IAssetLocator.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPathProvider.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformServices.h"
#include "base/platform/AppEventHooks.h"
#include "base/platform/MobileWindowSizing.h"
#include "base/platform/SdlAppEvents.h"
#include "base/platform/WindowIcon.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DeferredStartup.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/ShellHost.h"
#include "base/ui/Theme.h"
#include "common/StartupTiming.h"

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

namespace pbr {

namespace {

bool ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier,
                    float /*native_dp_ratio*/, bool priority) {
  return InputCoordinator::Instance().ProcessKeyDown(context, key, key_modifier, priority);
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
}

Application::~Application() {
  Shutdown();
}

std::string Application::AssetsPath(const std::string& relative) {
  return IAssetLocator::Instance().Resolve(relative);
}

bool Application::Initialize(const char* window_title) {
  if (initialized_) {
    return true;
  }

  if (!SessionStore::Instance().IsInitialized()) {
    log().error << "SessionStore not initialized";
    return false;
  }

  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();

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

  SessionStore::Instance().AddAppearanceListener([this](const std::string& appearance) {
    if (auto* ctx = Rml::GetContext("main")) {
      Theme::ApplyAppearance(ctx, Theme::ParseAppearance(appearance));
    }
  });

  SessionStore::Instance().AddThemeListener([this](const std::string& theme) {
    Theme::LoadBase(AssetsPath(theme));
    if (auto* ctx = Rml::GetContext("main")) {
      if (ctx->GetNumDocuments() > 0) {
        ctx->GetDocument(0)->UpdateDocument();
      }
    }
  });

  SessionStore::Instance().AddLanguageListener([](const std::string& language) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    SettingsController::Instance().RefreshLocalizedChrome();
    ShellHost::Instance().RequestSyncLayout(true);
    if (auto* ctx = Rml::GetContext("main")) {
      ApplyUiDocumentLanguage(ctx);
    }
  });

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

  ActionRouter::Instance().Attach(context);
  ActionRouter::Instance().SetModelDirtyCallback([](const std::string& model, const std::string& binding) {
    DataModelHost::Instance().Dirty(model, binding);
  });
  if (![&] {
        StartupPhase phase("SetupChatController");
        return SetupChatController(context);
      }()) {
    log().error << "SetupChatController failed";
    Rml::RemoveContext("main");
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

  ShellHost::Instance().SetSafeAreaInsetsFromPrefs(bootstrap.machine_prefs.safe_area.top,
                                                   bootstrap.machine_prefs.safe_area.bottom);
  ShellHost::Instance().RefreshSafeAreaInsets(context);
  ShellHost::Instance().SyncChromeMaterialPrefs(bootstrap.profile_prefs.reduce_transparency,
                                                bootstrap.profile_prefs.compact_chrome_frost);

  SessionStore::Instance().AddChromeMaterialListener([](bool reduce_transparency, bool compact_chrome_frost) {
    ShellHost::Instance().SyncChromeMaterialPrefs(reduce_transparency, compact_chrome_frost);
  });

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
    if (ShellHost::Instance().State().account_sheet_open ||
        ShellHost::Instance().State().nav_tab == NavTab::Me) {
      SettingsController::Instance().Tick();
    }
    if (ShellHost::Instance().State().nav_tab == NavTab::Contacts) {
      ContactsController::Instance().Tick();
    }
    UpdateChatController();
    ContextMenuHost::Instance().Update();
    ShellHost::Instance().Update(context);
    context->Update();
    // After Context::Update (which resets next_update_timeout): arm power-save for shell timers.
    ShellHost::Instance().NotifyFrameEnd(context);
    // Skip Clear/Present when the Android EGL surface is gone or size is not ready yet.
    if (Backend::CanRender()) {
      Backend::BeginFrame();
      context->Render();
      Backend::PresentFrame();
      if (!logged_first_present) {
        StartupMark("first_present");
        logged_first_present = true;
        BrowserThread::PostTask(BrowserThreadId::UI, []() { OnFirstPresentDeferredStartup(); });
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
  // Join notification watcher first so it cannot PostTask during teardown, and
  // so process exit does not std::terminate on an unjoined std::thread.
  ILocalNotifier::Instance().Shutdown();

  if (initialized_) {
#if RMLUI_SDL_VERSION_MAJOR >= 3
    Backend::SetLiveResizeHandler(nullptr, nullptr);
#endif
    ShutdownChatController();

    BrowserThread::RunUITasks();

    ActionRouter::Instance().Detach();
    Rml::RemoveContext("main");
#ifdef PPBROWSER_ENABLE_DEBUGGER
    Rml::Debugger::Shutdown();
#endif
    Rml::Shutdown();
    harfbuzz_font_engine_.reset();
    BrowserThread::SetUIWakeCallback(nullptr);
    Backend::Shutdown();

    log().info << "Shutdown complete";
    initialized_ = false;
  }

  // Always tear down runners — Initialize may have started them before failing.
  BrowserThread::Shutdown();
}

} // namespace pbr
