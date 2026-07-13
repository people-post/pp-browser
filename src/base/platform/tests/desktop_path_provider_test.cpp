#include "base/platform/DesktopPathProvider.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
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

void ExpectPathEq(const std::string& actual, const std::string& expected) {
  EXPECT_EQ(std::filesystem::path(actual), std::filesystem::path(expected));
}

} // namespace

#if defined(_WIN32)

TEST(DesktopPathProviderTest, ConfigDirUsesAppData) {
  ScopedEnv appdata("APPDATA", "C:/tmp/pp-browser-appdata");
  pbr::DesktopPathProvider provider;
  ExpectPathEq(provider.ConfigDir(), "C:/tmp/pp-browser-appdata/pp-browser");
}

TEST(DesktopPathProviderTest, DataDirUsesLocalAppData) {
  ScopedEnv local("LOCALAPPDATA", "C:/tmp/pp-browser-local");
  pbr::DesktopPathProvider provider;
  ExpectPathEq(provider.DataDir(""), "C:/tmp/pp-browser-local/pp-browser");
}

TEST(DesktopPathProviderTest, ExpandsTildeUnderUserProfile) {
  ScopedEnv profile("USERPROFILE", "C:/tmp/pp-browser-home");
  pbr::DesktopPathProvider provider;
  ExpectPathEq(provider.DataDir("~/custom-data"), "C:/tmp/pp-browser-home/custom-data");
}

#else

TEST(DesktopPathProviderTest, ConfigDirUsesXdgConfigHome) {
  ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
  ScopedEnv xdg("XDG_CONFIG_HOME", "/tmp/pp-browser-xdg-config");
  pbr::DesktopPathProvider provider;
  ExpectPathEq(provider.ConfigDir(), "/tmp/pp-browser-xdg-config/pp-browser");
}

TEST(DesktopPathProviderTest, DataDirUsesXdgDataHome) {
  ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
  ScopedEnv xdg("XDG_DATA_HOME", "/tmp/pp-browser-xdg-data");
  pbr::DesktopPathProvider provider;
  ExpectPathEq(provider.DataDir(""), "/tmp/pp-browser-xdg-data/pp-browser");
}

TEST(DesktopPathProviderTest, ExpandsTildeUnderHome) {
  ScopedEnv home("HOME", "/tmp/pp-browser-test-home");
  pbr::DesktopPathProvider provider;
  ExpectPathEq(provider.DataDir("~/custom-data"), "/tmp/pp-browser-test-home/custom-data");
}

#endif
