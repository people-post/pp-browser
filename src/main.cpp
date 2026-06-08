#include "app/Application.h"

#include <cstring>
#include <iostream>

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

  ppbrowser::Application app;
  if (!app.Initialize("pp-browser", 1280, 720, demo)) {
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
