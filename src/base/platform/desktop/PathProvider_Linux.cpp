#if !defined(_WIN32) && !defined(__APPLE__)

#include "base/platform/desktop/PathProviderImpl.h"

#include "base/platform/ExecutablePath.h"

#include <filesystem>

namespace pbr::desktop {

std::string ConfigDirImpl() {
  if (const std::string xdg = ReadEnv("XDG_CONFIG_HOME"); !xdg.empty()) {
    return (std::filesystem::path(xdg) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / ".config" / "pp-browser").string();
  }
  return "./pp-browser-config";
}

std::string DataDirImpl(const std::string& override_path) {
  if (!override_path.empty()) {
    return ExpandHome(override_path);
  }
  if (const std::string xdg = ReadEnv("XDG_DATA_HOME"); !xdg.empty()) {
    return (std::filesystem::path(xdg) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / ".local" / "share" / "pp-browser").string();
  }
  return "./pp-browser-data";
}

std::string CacheDirImpl(const std::string& data_dir) {
  if (const std::string xdg = ReadEnv("XDG_CACHE_HOME"); !xdg.empty()) {
    return (std::filesystem::path(xdg) / "pp-browser").string();
  }
  return (std::filesystem::path(data_dir) / "cache").string();
}

std::string PackagedBundleAssetsDirImpl() {
  const auto exe = ExecutablePath();
  if (exe.empty()) {
    return "assets";
  }
  return (exe.parent_path().parent_path() / "share" / "pp-browser" / "assets").lexically_normal().string();
}

} // namespace pbr::desktop

#endif
