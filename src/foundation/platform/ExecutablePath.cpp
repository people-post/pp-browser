#include "foundation/platform/ExecutablePath.h"

#include "foundation/platform/os/OsExecutablePath.h"

namespace pbr {

std::filesystem::path ExecutablePath() {
  return os::ExecutablePathImpl();
}

} // namespace pbr
