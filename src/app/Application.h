#pragma once

#include "app/Bootstrap.h"
#include "app/Config.h"
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

  bool Initialize(const char* window_title, DemoMode demo, const BootstrapResult& bootstrap);
  void Run();
  void Shutdown();

  static std::string AssetsPath(const std::string& relative);

  const BootstrapResult& Bootstrap() const { return bootstrap_; }

private:
  bool initialized_ = false;
  DemoMode demo_ = DemoMode::Chat;
  BootstrapResult bootstrap_;
};

} // namespace pbr
