#pragma once

#include "platform/IPathProvider.h"

namespace pbr {

// Reserved for a future iOS build; not selected at runtime on desktop or Android.
class IosPathProvider : public IPathProvider {
public:
  std::string ConfigDir() const override;
  std::string DataDir(const std::string& override_path) const override;
  std::string CacheDir(const std::string& data_dir) const override;
  std::string BundleAssetsDir() const override;
};

} // namespace pbr
