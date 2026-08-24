#include "base/platform/PlatformOpenFile.h"

#include "base/platform/PlatformOpenUrl.h"

#include <filesystem>

namespace pbr {

namespace {

std::string FilePathToUrl(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  std::filesystem::path file_path(path);
  std::string normalized = file_path.lexically_normal().string();
#if defined(_WIN32)
  for (char& ch : normalized) {
    if (ch == '\\') {
      ch = '/';
    }
  }
  return "file:///" + normalized;
#else
  return "file://" + normalized;
#endif
}

} // namespace

bool PlatformOpenFile(const std::string& path) {
  if (path.empty() || !std::filesystem::exists(path)) {
    return false;
  }
  return PlatformOpenUrl(FilePathToUrl(path));
}

} // namespace pbr
