#if defined(_WIN32)

#include "foundation/platform/os/OsThreadName.h"

#include <windows.h>

#include <string>

namespace pbr::os {

void SetCurrentThreadName(const std::string& name) {
  const std::wstring wide(name.begin(), name.end());
  (void)SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace pbr::os

#endif
