#include "platform/IosPathProvider.h"

namespace pbr {

std::string IosPathProvider::ConfigDir() const {
  return "./pp-browser-config";
}

std::string IosPathProvider::DataDir(const std::string& override_path) const {
  return override_path.empty() ? "./pp-browser-data" : override_path;
}

std::string IosPathProvider::CacheDir(const std::string& data_dir) const {
  return data_dir + "/cache";
}

std::string IosPathProvider::BundleAssetsDir() const {
  return "assets";
}

} // namespace pbr
