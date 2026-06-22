#include "app/Application.h"

#include "app/InputCoordinator.h"
#include "app/SessionStore.h"
#include "bindings/ActionRouter.h"
#include "demo/ChatDemo.h"
#include "platform/BrowserThread.h"
#include "platform/IAssetLocator.h"
#include "platform/Platform.h"
#include "platform/PlatformServices.h"
#include "platform/SdlAppEvents.h"
#include "ui/ShellHost.h"
#include "demo/DynamicRmlDemo.h"
#include "demo/SearchDemo.h"
#include "ui/DocumentLoader.h"
#include "ui/Theme.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

#include "RmlUi_Backend.h"

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

bool Application::Initialize(const char* window_title, DemoMode demo) {
  if (initialized_) {
    return true;
  }

  if (!SessionStore::Instance().IsInitialized()) {
    log().error << "SessionStore not initialized";
    return false;
  }

  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();
  demo_ = demo;

  const int width = bootstrap.machine_prefs.window.width;
  const int height = bootstrap.machine_prefs.window.height;

  int window_width = width;
  int window_height = height;
  if (Platform::IsMobile()) {
    ResolveMobileWindowSize(window_width, window_height);
  }

  log().info << "Initializing (demo=" << static_cast<int>(demo) << ", " << window_width << "x" << window_height << ")";

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

  auto* context = Rml::CreateContext("main", Rml::Vector2i(window_width, window_height));
  if (!context) {
    log().error << "Rml::CreateContext failed";
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

  Backend::SyncContext(context);

  SessionStore::Instance().AddThemeListener([this](const std::string& theme) {
    Theme::LoadBase(AssetsPath(theme));
    if (auto* ctx = Rml::GetContext("main")) {
      if (ctx->GetNumDocuments() > 0) {
        ctx->GetDocument(0)->UpdateDocument();
      }
    }
  });

  SdlAppEvents::Install();

  ActionRouter::Instance().Attach(context);
  if (demo == DemoMode::Search) {
    if (!SetupSearchDemo(context)) {
      log().error << "SetupSearchDemo failed";
      Rml::RemoveContext("main");
      Rml::Shutdown();
      Backend::Shutdown();
      return false;
    }
  } else if (demo == DemoMode::Hello) {
    DocumentLoader::LoadFile(context, AssetsPath("samples/hello.rml"));
  } else if (demo == DemoMode::Dynamic) {
    if (!SetupDynamicRmlDemo(context)) {
      log().error << "SetupDynamicRmlDemo failed";
      Rml::RemoveContext("main");
      Rml::Shutdown();
      Backend::Shutdown();
      return false;
    }
  } else if (!SetupChatDemo(context)) {
    log().error << "SetupChatDemo failed";
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
    if (demo_ == DemoMode::Chat) {
      UpdateChatDemo();
      ShellHost::Instance().Update(context);
    }
    context->Update();
    Backend::BeginFrame();
    context->Render();
    Backend::PresentFrame();
  }
}

void Application::Shutdown() {
  if (!initialized_) {
    return;
  }

  if (demo_ == DemoMode::Chat) {
    ShutdownChatDemo();
  }

  BrowserThread::RunUITasks();
  BrowserThread::Shutdown();

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

} // namespace pbr
