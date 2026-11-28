#pragma once

#include <string>

namespace pbr {

class AppPaths {
public:
  static std::string ConfigDir();
  static std::string DataDir(const std::string& override_path = {});
  static std::string CacheDir();
  static std::string ConfigFilePath();
  static void EnsureDirs(const std::string& profile_data_dir);
};

} // namespace pbr
