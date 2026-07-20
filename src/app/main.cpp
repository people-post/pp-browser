#include "app/Application.h"
#include "app/Bootstrap.h"
#include "base/data/SessionStore.h"
#include "common/Logger.h"
#include "base/platform/ProductBranding.h"
#include "base/platform/Platform.h"

#include <SDL3/SDL_main.h>

#include <cstring>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // Simulator/device debugging: always capture INFO+; --debug enables DEBUG.
  root.setLevel(debug_mode ? pbr::logging::Level::DEBUG : pbr::logging::Level::INFO);
  root.info << "Logging level set to " << (debug_mode ? "DEBUG" : "INFO");
#else
  root.setLevel(debug_mode ? pbr::logging::Level::DEBUG
                           : pbr::logging::Level::WARNING);
  root.info << "Logging level set to " << (debug_mode ? "DEBUG" : "WARNING");
#endif

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
    root.error << pbr::kProductName << ": failed to initialize.";
#if defined(__ANDROID__)
    root.error << " Check logcat for SDL/OpenGL errors.";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    root.error << " Check Console / Application Support/.../frame-debug.log for SDL/OpenGL errors.";
#else
    root.error << " If no window appears, reconfigure from a clean build: "
               << "rm -rf build && cmake -B build -S . && cmake --build build. "
               << "Ensure DISPLAY is set. On Linux install: libx11-dev and libgl-dev (see docs/ops/BUILD.md).";
#endif
    return 1;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
