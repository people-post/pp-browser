#if defined(_WIN32)

#include "base/platform/desktop/PathProviderImpl.h"

#include "base/platform/ExecutablePath.h"

#include <filesystem>

namespace pbr::desktop {

std::string ConfigDirImpl() {
  if (const std::string appdata = ReadEnv("APPDATA"); !appdata.empty()) {
    return (std::filesystem::path(appdata) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "AppData" / "Roaming" / "pp-browser").string();
  }
  return "./pp-browser-config";
}

std::string DataDirImpl(const std::string& override_path) {
  if (!override_path.empty()) {
    return ExpandHome(override_path);
  }
  if (const std::string local = ReadEnv("LOCALAPPDATA"); !local.empty()) {
    return (std::filesystem::path(local) / "pp-browser").string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "AppData" / "Local" / "pp-browser").string();
  }
  return "./pp-browser-data";
}

std::string CacheDirImpl(const std::string& data_dir) {
  if (const std::string local = ReadEnv("LOCALAPPDATA"); !local.empty()) {
    return (std::filesystem::path(local) / "pp-browser" / "cache").string();
  }
  return (std::filesystem::path(data_dir) / "cache").string();
}

std::string PackagedBundleAssetsDirImpl() {
  const auto exe = ExecutablePath();
  if (exe.empty()) {
    return "assets";
  }
  return (exe.parent_path() / "assets").lexically_normal().string();
}

} // namespace pbr::desktop

#endif
