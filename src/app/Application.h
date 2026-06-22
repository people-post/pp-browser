#pragma once

#include "app/Bootstrap.h"
#include "common/Module.h"

#include <optional>
#include <string>

namespace pbr {

enum class DemoMode {
  Chat,
  Search,
  Hello,
  Dynamic,
};

class Application : public Module {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  bool Initialize(const char* window_title, DemoMode demo);
  void Run();
  void Shutdown();

  static std::string AssetsPath(const std::string& relative);

private:
  bool initialized_ = false;
  DemoMode demo_ = DemoMode::Chat;
};

} // namespace pbr
