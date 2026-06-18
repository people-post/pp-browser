#include "app/Application.h"

#include "app/InputCoordinator.h"
#include "bindings/ActionRouter.h"
#include "demo/ChatDemo.h"
#include "platform/BrowserThread.h"
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

#ifdef PPBROWSER_ENABLE_DEBUGGER
#include <RmlUi/Debugger.h>
#endif

#include <filesystem>
#include <string>

namespace pbr {

namespace {

std::string JoinPath(const std::string& base, const std::string& relative) {
  std::filesystem::path path = std::filesystem::path(base) / relative;
  return path.lexically_normal().string();
}

bool ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier,
                    float /*native_dp_ratio*/, bool priority) {
  return InputCoordinator::Instance().ProcessKeyDown(context, key, key_modifier, priority);
}

} // namespace

Application::Application() {
  redirectLogger("Application");
}

Application::~Application() {
  Shutdown();
}

std::string Application::AssetsPath(const std::string& relative) {
#ifdef PP_BROWSER_ASSETS_DIR
  return JoinPath(PP_BROWSER_ASSETS_DIR, relative);
#else
  return JoinPath("assets", relative);
#endif
}

bool Application::Initialize(const char* window_title, int width, int height, DemoMode demo,
                             const AppConfig& config) {
  if (initialized_) {
    return true;
  }

  demo_ = demo;

  log().info << "Initializing (demo=" << static_cast<int>(demo) << ", " << width << "x" << height << ")";

  BrowserThread::Initialize();

  if (!Backend::Initialize(window_title, width, height, true)) {
    log().error << "Backend::Initialize failed (SDL/OpenGL window could not be created)";
    return false;
  }

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());

  if (!Rml::Initialise()) {
    log().error << "Rml::Initialise failed";
    Backend::Shutdown();
    return false;
  }

#ifdef PPBROWSER_ENABLE_DEBUGGER
  Rml::Debugger::Initialise(Rml::CreateContext("debugger", Rml::Vector2i(0, 0)));
#endif

  Theme::LoadBase(AssetsPath("themes/base.rcss"));
  Rml::LoadFontFace(AssetsPath("fonts/LatoLatin-Regular.ttf"));

  auto* context = Rml::CreateContext("main", Rml::Vector2i(width, height));
  if (!context) {
    log().error << "Rml::CreateContext failed";
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

  Backend::SyncContext(context);

  InputCoordinator::Instance().Clear();
  InputCoordinator::Instance().Register(KeyBinding{
      .key = Rml::Input::KI_ESCAPE,
      .action = []() {
        Backend::RequestExit();
        return false;
      },
      .priority = 100,
  });

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
  } else if (!SetupChatDemo(context, config)) {
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
