#include "app/Application.h"
#include "app/Bootstrap.h"
#include "base/data/SessionStore.h"
#include "common/Logger.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformLogDefaults.h"
#include "base/platform/PlatformStartupHints.h"
#include "base/platform/ProductBranding.h"

#include <SDL3/SDL_main.h>

#include <cstring>

int main(int argc, char** argv) {
  bool debug_mode = false;
  std::string profile_override;
  std::string pin;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
    } else if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile_override = argv[i + 1];
      ++i;
    } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
      pin = argv[i + 1];
      ++i;
    }
  }

  auto root = pbr::logging::getRootLogger();
  const auto default_level = pbr::DefaultRootLogLevel(debug_mode);
  root.setLevel(default_level);
  root.info << "Logging level set to "
            << (default_level == pbr::logging::Level::DEBUG   ? "DEBUG"
                : default_level == pbr::logging::Level::INFO ? "INFO"
                                                             : "WARNING");

  if (!pbr::Platform::EarlyInit()) {
    root.error << "Platform early init failed";
    return 1;
  }

  if (!profile_override.empty()) {
    root.warning << "Using profile override '" << profile_override
                 << "' (multi-profile UI is not shipped yet)";
  }

  pbr::BootstrapOptions options;
  options.argc = argc;
  options.argv = argv;
  options.profile_override = profile_override;
  options.pin = pin;

  auto bootstrap_result = pbr::Bootstrap::Run(options);
  if (!bootstrap_result) {
    root.error << pbr::kProductName << ": " << bootstrap_result.error().message;
    return 1;
  }

  pbr::SessionStore::Instance().Initialize(std::move(bootstrap_result.value()));

  pbr::Application app;
  if (!app.Initialize(pbr::kProductName)) {
    root.error << pbr::kProductName << ": failed to initialize." << pbr::InitFailureHint();
    return 1;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
