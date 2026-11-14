#include "foundation/platform/os/OsDeviceName.h"

#if !defined(_WIN32)

#include "common/DeviceName.h"

#include <unistd.h>

namespace pbr::os {

std::string SystemDeviceName() {
  char name[256] = {};
  if (gethostname(name, sizeof(name) - 1) != 0) {
    return {};
  }
  return NormalizeDeviceName(name);
}

} // namespace pbr::os

#endif // !_WIN32
