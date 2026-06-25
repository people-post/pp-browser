#pragma once

#include "base/platform/IPathProvider.h"

namespace pbr {

class AndroidPathProvider : public IPathProvider {
public:
  std::string ConfigDir() const override;
  std::string DataDir(const std::string& override_path) const override;
  std::string CacheDir(const std::string& data_dir) const override;
  std::string BundleAssetsDir() const override;

private:
  std::string RootDir() const;
};

} // namespace pbr
