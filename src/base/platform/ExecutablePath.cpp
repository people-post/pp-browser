#include "base/platform/ExecutablePath.h"

#include "base/platform/os/OsExecutablePath.h"

namespace pbr {

std::filesystem::path ExecutablePath() {
  return os::ExecutablePathImpl();
}

} // namespace pbr
