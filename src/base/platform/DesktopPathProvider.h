#pragma once

#include "base/platform/IPathProvider.h"

namespace pbr {

class DesktopPathProvider : public IPathProvider {
public:
  std::string ConfigDir() const override;
  std::string DataDir(const std::string& override_path) const override;
  std::string CacheDir(const std::string& data_dir) const override;
  std::string BundleAssetsDir() const override;
};

} // namespace pbr
