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
      } else if (std::strcmp(argv[i + 1], "dynamic") == 0) {
        demo = pbr::DemoMode::Dynamic;
      }
    }
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(debug_mode ? pbr::logging::Level::DEBUG
                           : pbr::logging::Level::WARNING);
  root.info << "Logging level set to " << (debug_mode ? "DEBUG" : "WARNING");

  auto config_result = pbr::Config::Load(argc, argv);
  if (!config_result) {
    root.error << "pp-browser: " << config_result.error().message;
    return 1;
  }
  const pbr::AppConfig config = config_result.value();

  pbr::Application app;
  if (!app.Initialize("pp-browser", 1280, 720, demo, config)) {
    root.error << "pp-browser: failed to initialize. "
               << "If no window appears, reconfigure from a clean build: "
               << "rm -rf build && cmake -B build -S . && cmake --build build. "
               << "Ensure DISPLAY is set. On Linux install: libx11-dev and libgl-dev (see docs/BUILD.md).";
    return 1;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
