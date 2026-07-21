#pragma once

#include "app/Bootstrap.h"
#include "common/Module.h"

#include <memory>
#include <optional>
#include <string>

class FontEngineInterfaceHarfBuzz;

namespace pbr {

class Application : public Module {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  bool Initialize(const char* window_title);
  void Run();
  void Shutdown();

  static std::string AssetsPath(const std::string& relative);

private:
  bool initialized_ = false;
  std::unique_ptr<FontEngineInterfaceHarfBuzz> harfbuzz_font_engine_;
};

} // namespace pbr
