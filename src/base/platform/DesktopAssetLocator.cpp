#include "platform/DesktopAssetLocator.h"

#include "platform/IPathProvider.h"

#include <filesystem>

namespace pbr {

std::string DesktopAssetLocator::Resolve(const std::string& relative) const {
  return (std::filesystem::path(IPathProvider::Instance().BundleAssetsDir()) / relative).lexically_normal().string();
}

} // namespace pbr
