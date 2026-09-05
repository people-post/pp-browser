#include "foundation/platform/IosAssetLocator.h"

#include "foundation/platform/IPathProvider.h"

#include <filesystem>

namespace pbr {

std::string IosAssetLocator::Resolve(const std::string& relative) const {
  return (std::filesystem::path(IPathProvider::Instance().BundleAssetsDir()) / relative)
      .lexically_normal()
      .string();
}

} // namespace pbr
