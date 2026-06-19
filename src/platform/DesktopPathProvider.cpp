#include "platform/DesktopPathProvider.h"

#include <cstdlib>
#include <filesystem>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

std::string ExpandHome(std::string path) {
  if (!path.empty() && path.front() == '~') {
    const char* home = std::getenv("HOME");
    if (!home) {
      home = std::getenv("USERPROFILE");
    }
    if (home) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

std::string HomeDir() {
  if (const char* home = std::getenv("HOME")) {
    return home;
  }
  if (const char* profile = std::getenv("USERPROFILE")) {
    return profile;
  }
  return {};
}

} // namespace

std::string DesktopPathProvider::ConfigDir() const {
#if defined(_WIN32)
  if (const std::string appdata = ReadEnv("APPDATA"); !appdata.empty()) {
    return (std::filesystem::path(appdata) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "AppData" / "Roaming" / "pp-browser").string();
  }
  return "./pp-browser-config";
#elif defined(__APPLE__)
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "Library" / "Application Support" / "pp-browser").string();
  }
  return "./pp-browser-config";
#else
  if (const std::string xdg = ReadEnv("XDG_CONFIG_HOME"); !xdg.empty()) {
    return (std::filesystem::path(xdg) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / ".config" / "pp-browser").string();
  }
  return "./pp-browser-config";
#endif
}

std::string DesktopPathProvider::DataDir(const std::string& override_path) const {
  if (!override_path.empty()) {
    return ExpandHome(override_path);
  }
#if defined(_WIN32)
  if (const std::string local = ReadEnv("LOCALAPPDATA"); !local.empty()) {
    return (std::filesystem::path(local) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "AppData" / "Local" / "pp-browser").string();
  }
  return "./pp-browser-data";
#elif defined(__APPLE__)
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "Library" / "Application Support" / "pp-browser" / "data").string();
  }
  return "./pp-browser-data";
#else
  if (const std::string xdg = ReadEnv("XDG_DATA_HOME"); !xdg.empty()) {
    return (std::filesystem::path(xdg) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / ".local" / "share" / "pp-browser").string();
  }
  return "./pp-browser-data";
#endif
}

std::string DesktopPathProvider::CacheDir(const std::string& data_dir) const {
#if defined(_WIN32)
  if (const std::string local = ReadEnv("LOCALAPPDATA"); !local.empty()) {
    return (std::filesystem::path(local) / "pp-browser" / "cache").string();
  }
#elif defined(__APPLE__)
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "Library" / "Caches" / "pp-browser").string();
  }
#else
  if (const std::string xdg = ReadEnv("XDG_CACHE_HOME"); !xdg.empty()) {
    return (std::filesystem::path(xdg) / "pp-browser").string();
  }
#endif
  return (std::filesystem::path(data_dir) / "cache").string();
}

std::string DesktopPathProvider::BundleAssetsDir() const {
#ifdef PP_BROWSER_ASSETS_DIR
  return PP_BROWSER_ASSETS_DIR;
#else
  return "assets";
#endif
}

} // namespace pbr
