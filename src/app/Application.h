#pragma once

#include <string>

namespace ppbrowser {

class Application {
public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  bool Initialize(const char* window_title, int width, int height, bool search_demo = false);
  void Run();
  void Shutdown();

  static std::string AssetsPath(const std::string& relative);

private:
  bool initialized_ = false;
};

} // namespace ppbrowser
