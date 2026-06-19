#include "platform/ExecutablePath.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <vector>

namespace pbr {

std::filesystem::path ExecutablePath() {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) {
      return {};
    }
    if (len < buffer.size()) {
      buffer.resize(len);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> path_buffer(size);
  if (_NSGetExecutablePath(path_buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::weakly_canonical(std::filesystem::path(path_buffer.data()));
#else
  std::vector<char> path_buffer(4096);
  const ssize_t len = readlink("/proc/self/exe", path_buffer.data(), path_buffer.size() - 1);
  if (len <= 0) {
    return {};
  }
  path_buffer[static_cast<size_t>(len)] = '\0';
  return std::filesystem::weakly_canonical(std::filesystem::path(path_buffer.data()));
#endif
}

} // namespace pbr
