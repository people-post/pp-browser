#include "platform/DesktopPathProvider.h"

#include <cassert>
#include <cstdlib>
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
    setenv(name, value, 1);
  }

  ~ScopedEnv() {
    if (previous_) {
      setenv(name_, previous_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

private:
  const char* name_;
  std::optional<std::string> previous_;
};

} // namespace

int main() {
  {
    ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
    ScopedEnv xdg("XDG_CONFIG_HOME", "/tmp/pp-browser-xdg-config");
    pbr::DesktopPathProvider provider;
    assert(provider.ConfigDir() == "/tmp/pp-browser-xdg-config/pp-browser");
  }

  {
    ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
    ScopedEnv xdg("XDG_DATA_HOME", "/tmp/pp-browser-xdg-data");
    pbr::DesktopPathProvider provider;
    assert(provider.DataDir("") == "/tmp/pp-browser-xdg-data/pp-browser");
  }

  {
    ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
    pbr::DesktopPathProvider provider;
    assert(provider.DataDir("~/custom-data") == "/tmp/pp-browser-test-home/custom-data");
  }

  std::cout << "desktop_path_provider_test ok\n";
  return 0;
}
