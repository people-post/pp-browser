#if defined(__APPLE__)

#include "base/platform/os/OsExecutablePath.h"

#include <mach-o/dyld.h>

#include <vector>

namespace pbr::os {

std::filesystem::path ExecutablePathImpl() {
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> path_buffer(size);
  if (_NSGetExecutablePath(path_buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::weakly_canonical(std::filesystem::path(path_buffer.data()));
}

} // namespace pbr::os

#endif
