#include "foundation/platform/PlatformOpenFile.h"

#include "foundation/platform/PlatformOpenUrl.h"

#include "common/PathUtf8.h"

#include <filesystem>

namespace pbr {

namespace {

std::string FilePathToUrl(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  std::string normalized = PathToUtf8(PathFromUtf8(path).lexically_normal());
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
  if (path.empty() || !std::filesystem::exists(PathFromUtf8(path))) {
    return false;
  }
  return PlatformOpenUrl(FilePathToUrl(path));
}

void SetPlatformOpenFileSoundHooks(std::function<void()> /*begin*/, std::function<void()> /*end*/) {}

void PlatformForgetOpenedFiles() {} // the system app is handed the view itself: nothing else to remove

} // namespace pbr
