#pragma once

#include "app/Bootstrap.h"
#include "common/Error.h"
#include "common/Module.h"

#include <memory>
#include <optional>
#include <string>

class FontEngineInterfaceHarfBuzz;

namespace pbr {

class ConfigApplyBridge;
class MessagingHub;

class Application : public Module {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  bool Initialize(const char* window_title);
  void Run();
  void Shutdown();

  MessagingHub& Messaging();

  /** Tear down messaging hub + profile secrets when initialized. */
  void ShutdownMessaging();
  /** Wipe active profile data dir and reinitialize secrets + hub (app-owned lifecycle). */
  Roe<void> ResetActiveProfile();

  static std::string AssetsPath(const std::string& relative);

private:
  bool initialized_ = false;
  std::unique_ptr<MessagingHub> messaging_;
  std::unique_ptr<ConfigApplyBridge> config_apply_;
  std::unique_ptr<FontEngineInterfaceHarfBuzz> harfbuzz_font_engine_;
};

} // namespace pbr
