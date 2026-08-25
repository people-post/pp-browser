#if defined(_WIN32)

#include "base/platform/desktop/PathProviderImpl.h"

#include "base/platform/DeploymentProfile.h"
#include "base/platform/ExecutablePath.h"

#include <filesystem>

namespace pbr::desktop {

std::string ConfigDirImpl() {
  const std::string product = ProductDirBasename();
  if (const std::string appdata = ReadEnv("APPDATA"); !appdata.empty()) {
    return (std::filesystem::path(appdata) / product).string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "AppData" / "Roaming" / product).string();
  }
  return "./pp-browser-config";
}

std::string DataDirImpl(const std::string& override_path) {
  if (!override_path.empty()) {
    return ExpandHome(override_path);
  }
  const std::string product = ProductDirBasename();
  if (const std::string local = ReadEnv("LOCALAPPDATA"); !local.empty()) {
    return (std::filesystem::path(local) / product).string();
  }
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "AppData" / "Local" / product).string();
  }
  return "./pp-browser-data";
}

std::string CacheDirImpl(const std::string& data_dir) {
  const std::string product = ProductDirBasename();
  if (const std::string local = ReadEnv("LOCALAPPDATA"); !local.empty()) {
    return (std::filesystem::path(local) / product / "cache").string();
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
