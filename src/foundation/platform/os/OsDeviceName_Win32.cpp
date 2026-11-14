#include "foundation/platform/os/OsDeviceName.h"

#if defined(_WIN32)

#include "common/DeviceName.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pbr::os {

std::string SystemDeviceName() {
  wchar_t name[256] = {};
  DWORD size = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
  if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size)) {
    return {};
  }
  char narrow[1024] = {};
  if (WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr) <= 0) {
    return {};
  }
  return NormalizeDeviceName(narrow);
}

} // namespace pbr::os

#endif // _WIN32
