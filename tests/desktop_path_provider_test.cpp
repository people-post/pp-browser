#include "platform/DesktopPathProvider.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

class ScopedEnv {
public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* prev = std::getenv(name)) {
      previous_ = prev;
    }
#if defined(_WIN32)
    assignment_ = std::string(name) + "=" + value;
    _putenv(assignment_.c_str());
#else
    setenv(name, value, 1);
#endif
  }

  ~ScopedEnv() {
    if (previous_) {
#if defined(_WIN32)
      assignment_ = std::string(name_) + "=" + *previous_;
      _putenv(assignment_.c_str());
#else
      setenv(name_, previous_->c_str(), 1);
#endif
    } else {
#if defined(_WIN32)
      assignment_ = std::string(name_) + "=";
      _putenv(assignment_.c_str());
#else
      unsetenv(name_);
#endif
    }
  }

private:
  const char* name_;
  std::optional<std::string> previous_;
  std::string assignment_;
};

void AssertPathEq(const std::string& actual, const std::string& expected) {
  assert(std::filesystem::path(actual) == std::filesystem::path(expected));
}

} // namespace

int main() {
#if defined(_WIN32)
  {
    ScopedEnv appdata("APPDATA", "C:/tmp/pp-browser-appdata");
    pbr::DesktopPathProvider provider;
    AssertPathEq(provider.ConfigDir(), "C:/tmp/pp-browser-appdata/pp-browser");
  }

  {
    ScopedEnv local("LOCALAPPDATA", "C:/tmp/pp-browser-local");
    pbr::DesktopPathProvider provider;
    AssertPathEq(provider.DataDir(""), "C:/tmp/pp-browser-local/pp-browser");
  }

  {
    ScopedEnv profile("USERPROFILE", "C:/tmp/pp-browser-home");
    pbr::DesktopPathProvider provider;
    AssertPathEq(provider.DataDir("~/custom-data"), "C:/tmp/pp-browser-home/custom-data");
  }
#else
  {
    ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
    ScopedEnv xdg("XDG_CONFIG_HOME", "/tmp/pp-browser-xdg-config");
    pbr::DesktopPathProvider provider;
    AssertPathEq(provider.ConfigDir(), "/tmp/pp-browser-xdg-config/pp-browser");
  }

  {
    ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
    ScopedEnv xdg("XDG_DATA_HOME", "/tmp/pp-browser-xdg-data");
    pbr::DesktopPathProvider provider;
    AssertPathEq(provider.DataDir(""), "/tmp/pp-browser-xdg-data/pp-browser");
  }

  {
    ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
    pbr::DesktopPathProvider provider;
    AssertPathEq(provider.DataDir("~/custom-data"), "/tmp/pp-browser-test-home/custom-data");
  }
#endif

  std::cout << "desktop_path_provider_test ok\n";
  return 0;
}
