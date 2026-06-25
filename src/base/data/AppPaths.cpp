#include "base/data/AppPaths.h"

#include "base/platform/IPathProvider.h"

#include <filesystem>

namespace pbr {

namespace {

std::string g_data_dir_override;

} // namespace

std::string AppPaths::ConfigDir() {
  return IPathProvider::Instance().ConfigDir();
}

std::string AppPaths::DataDir(const std::string& override_path) {
  if (!override_path.empty()) {
    g_data_dir_override = override_path;
  }
  return IPathProvider::Instance().DataDir(g_data_dir_override);
}

std::string AppPaths::CacheDir() {
  return IPathProvider::Instance().CacheDir(DataDir());
}

std::string AppPaths::ConfigFilePath() {
  return (std::filesystem::path(ConfigDir()) / "config.json").string();
}

void AppPaths::EnsureDirs(const std::string& profile_data_dir) {
  std::error_code ec;
  std::filesystem::create_directories(ConfigDir(), ec);
  std::filesystem::create_directories(DataDir(), ec);
  std::filesystem::create_directories(CacheDir(), ec);
  std::filesystem::create_directories(profile_data_dir, ec);
}

} // namespace pbr
