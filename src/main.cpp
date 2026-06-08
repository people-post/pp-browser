#include "app/Application.h"

#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  bool search_demo = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--demo") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "search") == 0) {
      search_demo = true;
    }
  }

  ppbrowser::Application app;
  if (!app.Initialize("pp-browser", 1280, 720, search_demo)) {
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
