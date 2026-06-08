#include "app/Application.h"

#include "bindings/ActionRouter.h"
#include "demo/ChatDemo.h"
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
#include <iostream>
#include <string>

namespace ppbrowser {

namespace {

std::string JoinPath(const std::string& base, const std::string& relative) {
  std::filesystem::path path = std::filesystem::path(base) / relative;
  return path.lexically_normal().string();
}

bool ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key,
                    int key_modifier, float /*native_dp_ratio*/, bool priority) {
  if (!priority) {
    return true;
  }
  if (key == Rml::Input::KI_ESCAPE) {
    Backend::RequestExit();
    return false;
  }
  return true;
}

} // namespace

Application::Application() = default;

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

  if (!Backend::Initialize(window_title, width, height, true)) {
    std::cerr << "Backend::Initialize failed (SDL/OpenGL window could not be created).\n";
    return false;
  }

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());

  if (!Rml::Initialise()) {
    std::cerr << "Rml::Initialise failed.\n";
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
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

  ActionRouter::Instance().Attach(context);
  if (demo == DemoMode::Search) {
    if (!SetupSearchDemo(context)) {
      Rml::RemoveContext("main");
      Rml::Shutdown();
      Backend::Shutdown();
      return false;
    }
  } else if (demo == DemoMode::Hello) {
    DocumentLoader::LoadFile(context, AssetsPath("samples/hello.rml"));
  } else if (!SetupChatDemo(context, config)) {
    Rml::RemoveContext("main");
    Rml::Shutdown();
    Backend::Shutdown();
    return false;
  }

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
    if (demo_ == DemoMode::Chat) {
      UpdateChatDemo();
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

  ActionRouter::Instance().Detach();
  Rml::RemoveContext("main");
#ifdef PPBROWSER_ENABLE_DEBUGGER
  Rml::Debugger::Shutdown();
#endif
  Rml::Shutdown();
  Backend::Shutdown();

  initialized_ = false;
}

} // namespace ppbrowser
