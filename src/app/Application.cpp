#include "app/Application.h"

#include "base/ui/InputCoordinator.h"
#include "base/ui/ContextMenuHost.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "feature/ai/bindings/ActionRouter.h"
#include "feature/chat/ChatController.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/IAssetLocator.h"
#include "base/platform/IPathProvider.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformServices.h"
#include "base/platform/SdlAppEvents.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/ShellHost.h"
#include "base/ui/Theme.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/TextLoupe.h>

#include "RmlUi_Backend.h"
#include "RmlUi_Renderer_GL3.h"
#include "TextLoupeRenderer.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#include <SDL3/SDL.h>
#endif

#ifdef PPBROWSER_ENABLE_DEBUGGER
#include <RmlUi/Debugger.h>
#endif

namespace pbr {

namespace {

bool ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier,
                    float /*native_dp_ratio*/, bool priority) {
  return InputCoordinator::Instance().ProcessKeyDown(context, key, key_modifier, priority);
}

void ResolveMobileWindowSize(int& width, int& height) {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  if (!SDL_WasInit(SDL_INIT_VIDEO)) {
    SDL_InitSubSystem(SDL_INIT_VIDEO);
  }
  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
  if (mode && mode->w > 0 && mode->h > 0) {
    width = mode->w;
    height = mode->h;
  }
#else
  (void)width;
  (void)height;
#endif
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

  if (!Backend::Initialize(window_title, window_width, window_height, true)) {
    log().error << "Backend::Initialize failed (SDL/OpenGL window could not be created)";
    return false;
  }

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());

  if (Rml::FileInterface* packaged_files = PlatformServices::PackagedFileInterface()) {
    Rml::SetFileInterface(packaged_files);
  }

  if (!Rml::Initialise()) {
    log().error << "Rml::Initialise failed";
    Backend::Shutdown();
    return false;
  }

#ifdef PPBROWSER_ENABLE_DEBUGGER
  Rml::Debugger::Initialise(Rml::CreateContext("debugger", Rml::Vector2i(0, 0)));
#endif

  const std::string theme_path = AssetsPath(bootstrap.profile_prefs.theme);
  Theme::LoadBase(theme_path);
  Rml::LoadFontFace(AssetsPath("fonts/LatoLatin-Regular.ttf"));
  // CJK fallback for Simplified Chinese UI (and other non-Latin glyphs).
  Rml::LoadFontFace(AssetsPath("fonts/NotoSansSC-Regular.subset.ttf"), true);

  if (auto loaded = LocalizationService::Instance().LoadFromAssets(IPathProvider::Instance().BundleAssetsDir());
      !loaded) {
    log().warning << "Localization catalogs failed to load: " << loaded.error().message;
  }
  LocalizationService::Instance().SetPreferredLanguage(bootstrap.profile_prefs.language);

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
  });

  SdlAppEvents::Install();

  ContextMenuHost::Instance().Install(context);

  ActionRouter::Instance().Attach(context);
  if (!SetupChatController(context)) {
    log().error << "SetupChatController failed";
    Rml::RemoveContext("main");
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

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

  while (Backend::ProcessEvents(context, ProcessKeyDown, true)) {
    BrowserThread::RunUITasks();
    UpdateChatController();
    ContextMenuHost::Instance().Update();
    ShellHost::Instance().Update(context);
    context->Update();
    // Skip Clear/Present when the Android EGL surface is gone or size is not ready yet.
    if (Backend::CanRender()) {
      Backend::BeginFrame();
      context->Render();
      Backend::PresentFrame();
    }
  }
}

void Application::Shutdown() {
  if (initialized_) {
    ShutdownChatController();

    BrowserThread::RunUITasks();

    ActionRouter::Instance().Detach();
    Rml::RemoveContext("main");
#ifdef PPBROWSER_ENABLE_DEBUGGER
    Rml::Debugger::Shutdown();
#endif
    Rml::Shutdown();
    Backend::Shutdown();

    log().info << "Shutdown complete";
    initialized_ = false;
  }

  // Always tear down runners — Initialize may have started them before failing.
  BrowserThread::Shutdown();
}

} // namespace pbr
