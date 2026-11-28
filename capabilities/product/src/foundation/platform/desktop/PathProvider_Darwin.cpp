#if defined(__APPLE__)

#include "foundation/platform/desktop/PathProviderImpl.h"

#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/ExecutablePath.h"

#include <filesystem>

namespace pbr::desktop {

std::string ConfigDirImpl() {
  const std::string product = ProductDirBasename();
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "Library" / "Application Support" / product).string();
  }
  return "./pp-browser-config";
}

std::string DataDirImpl(const std::string& override_path) {
  if (!override_path.empty()) {
    return ExpandHome(override_path);
  }
  const std::string product = ProductDirBasename();
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "Library" / "Application Support" / product / "data").string();
  }
  return "./pp-browser-data";
}

std::string CacheDirImpl(const std::string& data_dir) {
  const std::string product = ProductDirBasename();
  const std::string home = HomeDir();
  if (!home.empty()) {
    return (std::filesystem::path(home) / "Library" / "Caches" / product).string();
  }
  return (std::filesystem::path(data_dir) / "cache").string();
}

std::string PackagedBundleAssetsDirImpl() {
  const auto exe = ExecutablePath();
  if (exe.empty()) {
    return "assets";
  }
  return (exe.parent_path().parent_path() / "Resources" / "assets").lexically_normal().string();
}

} // namespace pbr::desktop

#endif
