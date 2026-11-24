// iOS and Android: a process cannot start itself again; the in-process path applies.
#include "foundation/platform/os/OsRestart.h"

namespace pbr::os {

bool RestartSupported() {
  return false;
}

bool RestartProcess(const RestartPlan& /*plan*/, std::string& error) {
  error = "restart is not supported on this platform";
  return false;
}

} // namespace pbr::os
