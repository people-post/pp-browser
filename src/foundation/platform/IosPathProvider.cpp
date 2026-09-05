#include "foundation/platform/IosPathProvider.h"

#include "foundation/platform/DeploymentProfile.h"

#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#endif

namespace pbr {

namespace {

std::string JoinPath(const std::filesystem::path& base, const std::string& leaf) {
  return (base / leaf).lexically_normal().string();
}

std::string RootDir() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (char* pref = SDL_GetPrefPath("dev.pp-browser", ProductDirBasename())) {
    const std::string path(pref);
    SDL_free(pref);
    return path;
  }
#endif
  return "./pp-browser-data";
}

} // namespace

std::string IosPathProvider::ConfigDir() const {
  return JoinPath(std::filesystem::path(RootDir()), "config");
}

std::string IosPathProvider::DataDir(const std::string& override_path) const {
  if (!override_path.empty()) {
    return override_path;
  }
  return JoinPath(std::filesystem::path(RootDir()), "data");
}

std::string IosPathProvider::CacheDir(const std::string& data_dir) const {
  return JoinPath(std::filesystem::path(data_dir), "cache");
}

std::string IosPathProvider::BundleAssetsDir() const {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  if (const char* base = SDL_GetBasePath()) {
    return JoinPath(std::filesystem::path(base), "assets");
  }
#endif
  // Fallback: SDL chdirs into the .app bundle before main.
  return "assets";
}

} // namespace pbr
