#pragma once

#include "base/platform/IPathProvider.h"

namespace pbr {

// Selected at runtime on iPhone/iPad (TARGET_OS_IPHONE).
class IosPathProvider : public IPathProvider {
public:
  std::string ConfigDir() const override;
  std::string DataDir(const std::string& override_path) const override;
  std::string CacheDir(const std::string& data_dir) const override;
  std::string BundleAssetsDir() const override;
};

} // namespace pbr
