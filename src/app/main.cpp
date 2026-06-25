#include "app/Application.h"
#include "app/Bootstrap.h"
#include "base/data/SessionStore.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "common/Logger.h"
#include "base/platform/Platform.h"

#include <SDL3/SDL_main.h>

#include <cstring>

namespace {

pbr::Libp2pHost g_libp2p_host;

} // namespace

int main(int argc, char** argv) {
  bool debug_mode = false;
  pbr::DemoMode demo = pbr::DemoMode::Chat;
  std::string profile_override;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
    } else if (std::strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
      if (std::strcmp(argv[i + 1], "search") == 0) {
        demo = pbr::DemoMode::Search;
      } else if (std::strcmp(argv[i + 1], "hello") == 0) {
        demo = pbr::DemoMode::Hello;
      } else if (std::strcmp(argv[i + 1], "dynamic") == 0) {
        demo = pbr::DemoMode::Dynamic;
      }
      ++i;
    } else if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile_override = argv[i + 1];
      ++i;
    }
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(debug_mode ? pbr::logging::Level::DEBUG
                           : pbr::logging::Level::WARNING);
  root.info << "Logging level set to " << (debug_mode ? "DEBUG" : "WARNING");

  if (g_libp2p_host.IsAvailable()) {
    root.info << "libp2p linked";
  }

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

  auto bootstrap_result = pbr::Bootstrap::Run(options);
  if (!bootstrap_result) {
    root.error << "pp-browser: " << bootstrap_result.error().message;
    return 1;
  }

  pbr::SessionStore::Instance().Initialize(std::move(bootstrap_result.value()));

  pbr::Application app;
  if (!app.Initialize("pp-browser", demo)) {
    root.error << "pp-browser: failed to initialize.";
#if defined(__ANDROID__)
    root.error << " Check logcat for SDL/OpenGL errors.";
#else
    root.error << " If no window appears, reconfigure from a clean build: "
               << "rm -rf build && cmake -B build -S . && cmake --build build. "
               << "Ensure DISPLAY is set. On Linux install: libx11-dev and libgl-dev (see docs/BUILD.md).";
#endif
    return 1;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
