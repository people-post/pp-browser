#include "base/platform/DesktopPathProvider.h"

#include "base/platform/desktop/PathProviderImpl.h"

namespace pbr {

std::string DesktopPathProvider::ConfigDir() const {
  return desktop::ConfigDirImpl();
}

std::string DesktopPathProvider::DataDir(const std::string& override_path) const {
  return desktop::DataDirImpl(override_path);
}

std::string DesktopPathProvider::CacheDir(const std::string& data_dir) const {
  return desktop::CacheDirImpl(data_dir);
}

std::string DesktopPathProvider::BundleAssetsDir() const {
#if defined(PP_BROWSER_PACKAGED_BUILD)
  return desktop::PackagedBundleAssetsDirImpl();
#else
#ifdef PP_BROWSER_ASSETS_DIR
  return PP_BROWSER_ASSETS_DIR;
#else
  return "assets";
#endif
#endif
}

} // namespace pbr
