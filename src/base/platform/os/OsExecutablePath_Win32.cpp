#if defined(_WIN32)

#include "base/platform/os/OsExecutablePath.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

namespace pbr::os {

std::filesystem::path ExecutablePathImpl() {
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
}

} // namespace pbr::os

#endif
