#pragma once

#include <string>

namespace pbr {

class IPathProvider {
public:
  virtual ~IPathProvider() = default;

  virtual std::string ConfigDir() const = 0;
  virtual std::string DataDir(const std::string& override_path) const = 0;
  virtual std::string CacheDir(const std::string& data_dir) const = 0;
  virtual std::string BundleAssetsDir() const = 0;

  static IPathProvider& Instance();
  static void SetInstance(IPathProvider* provider);
};

} // namespace pbr
