#pragma once

#include "app/Config.h"
#include "common/Module.h"

#include <string>

namespace pbr {

enum class DemoMode {
  Chat,
  Search,
  Hello,
};

class Application : public Module {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  bool Initialize(const char* window_title, int width, int height, DemoMode demo = DemoMode::Chat,
                  const AppConfig& config = Config::DefaultOllama());
  void Run();
  void Shutdown();

  static std::string AssetsPath(const std::string& relative);

private:
  bool initialized_ = false;
  DemoMode demo_ = DemoMode::Chat;
};

} // namespace pbr
