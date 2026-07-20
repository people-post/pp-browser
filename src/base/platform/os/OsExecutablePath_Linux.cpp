#if !defined(_WIN32) && !defined(__APPLE__)

#include "base/platform/os/OsExecutablePath.h"

#include <unistd.h>

#include <vector>

namespace pbr::os {

std::filesystem::path ExecutablePathImpl() {
  std::vector<char> path_buffer(4096);
  const ssize_t len = readlink("/proc/self/exe", path_buffer.data(), path_buffer.size() - 1);
  if (len <= 0) {
    return {};
  }
  path_buffer[static_cast<size_t>(len)] = '\0';
  return std::filesystem::weakly_canonical(std::filesystem::path(path_buffer.data()));
}

} // namespace pbr::os

#endif
