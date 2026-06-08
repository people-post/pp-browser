#include "app/Application.h"
#include "app/Config.h"
#include "log/Logger.h"

#include <cstring>

int main(int argc, char** argv) {
  bool debug_mode = false;
  pbr::DemoMode demo = pbr::DemoMode::Chat;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
    } else if (std::strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
      if (std::strcmp(argv[i + 1], "search") == 0) {
        demo = pbr::DemoMode::Search;
      } else if (std::strcmp(argv[i + 1], "hello") == 0) {
        demo = pbr::DemoMode::Hello;
      }
    }
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(debug_mode ? pbr::logging::Level::DEBUG
                           : pbr::logging::Level::WARNING);
  root.info << "Logging level set to " << (debug_mode ? "DEBUG" : "WARNING");

  pbr::AppConfig config;
  try {
    config = pbr::Config::Load(argc, argv);
  } catch (const std::exception& e) {
    root.error << "pp-browser: " << e.what();
    return 1;
  }

  pbr::Application app;
  if (!app.Initialize("pp-browser", 1280, 720, demo, config)) {
    root.error << "pp-browser: failed to initialize. "
               << "If no window appears, rebuild with X11 support: "
               << "rm -rf build/_deps/sdl3-build build/_deps/sdl3-src && "
               << "cmake -B build -S . && cmake --build build. "
               << "Ensure DISPLAY is set. On Linux install: libx11-dev and libgl-dev (see docs/BUILD.md).";
    return 1;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
