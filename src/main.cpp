#include "app/Application.h"
#include "app/Config.h"

#include <cstring>
#include <filesystem>
#include <iostream>

namespace {

std::string ConfigPath(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }

  if (const char* env = std::getenv("PP_BROWSER_CONFIG")) {
    return env;
  }

  if (std::filesystem::exists("config.json")) {
    return "config.json";
  }

  return {};
}

ppbrowser::AppConfig LoadConfig(int argc, char** argv) {
  const std::string path = ConfigPath(argc, argv);
  if (path.empty()) {
    std::cout << "No config.json found; using Ollama defaults (http://localhost:11434/v1).\n"
              << "Copy config.json.example to config.json to customize the model.\n";
    return ppbrowser::Config::DefaultOllama();
  }

  if (auto config = ppbrowser::Config::LoadFromFile(path)) {
    std::cout << "Loaded config from " << path << " (model: " << config->llm.model << ").\n";
    return *config;
  }

  throw std::runtime_error("Failed to read config: " + path);
}

} // namespace

int main(int argc, char** argv) {
  ppbrowser::DemoMode demo = ppbrowser::DemoMode::Chat;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
      if (std::strcmp(argv[i + 1], "search") == 0) {
        demo = ppbrowser::DemoMode::Search;
      } else if (std::strcmp(argv[i + 1], "hello") == 0) {
        demo = ppbrowser::DemoMode::Hello;
      }
    }
  }

  ppbrowser::AppConfig config;
  try {
    config = LoadConfig(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "pp-browser: " << e.what() << "\n";
    return 1;
  }

  ppbrowser::Application app;
  if (!app.Initialize("pp-browser", 1280, 720, demo, config)) {
    std::cerr << "pp-browser: failed to initialize.\n"
              << "If no window appears, rebuild with X11 support:\n"
              << "  rm -rf build/_deps/sdl3-build build/_deps/sdl3-src\n"
              << "  cmake -B build -S .\n"
              << "  cmake --build build\n"
              << "Ensure DISPLAY is set. On Linux install: libx11-dev and libgl-dev (see docs/BUILD.md).\n";
    return 1;
  }
  app.Run();
  app.Shutdown();
  return 0;
}
