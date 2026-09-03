#include "foundation/platform/AndroidPathProvider.h"

#include "foundation/platform/DeploymentProfile.h"

#include <filesystem>
#include <string>

#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#endif

namespace pbr {

namespace {

std::string JoinPath(const std::filesystem::path& base, const std::string& leaf) {
  return (base / leaf).lexically_normal().string();
}

} // namespace

std::string AndroidPathProvider::RootDir() const {
#if defined(__ANDROID__)
  if (const char* internal = SDL_GetAndroidInternalStoragePath()) {
    return JoinPath(std::filesystem::path(internal), ProductDirBasename());
  }
#endif
  return ProductDirBasename();
}

std::string AndroidPathProvider::ConfigDir() const {
  return JoinPath(std::filesystem::path(RootDir()), "config");
}

std::string AndroidPathProvider::DataDir(const std::string& override_path) const {
  if (!override_path.empty()) {
    return override_path;
  }
  return JoinPath(std::filesystem::path(RootDir()), "data");
}

std::string AndroidPathProvider::CacheDir(const std::string& data_dir) const {
#if defined(__ANDROID__)
  if (const char* cache = SDL_GetAndroidCachePath()) {
    return JoinPath(std::filesystem::path(cache), ProductDirBasename());
  }
#endif
  return JoinPath(std::filesystem::path(data_dir), "cache");
}

std::string AndroidPathProvider::BundleAssetsDir() const {
  // APK assets are opened by relative path via AndroidFileInterface / SDL asset I/O.
  return "";
}

} // namespace pbr
