#include "foundation/platform/os/OsProtectedData.h"

#include <atomic>
#include <chrono>

namespace pbr::os {

// No protected-data state here (desktop; Android until its own file, #21 PR 2): always readable, so the
// handlers never fire; no boot session id, and the steady clock stands in for the monotonic one.

bool ProtectedDataAvailable() {
  return true;
}

ProtectedDataHandlerId OnProtectedDataAvailable(std::function<void()> /*fn*/) {
  static std::atomic<ProtectedDataHandlerId> next{1};
  return next.fetch_add(1);
}

void RemoveProtectedDataHandler(ProtectedDataHandlerId /*id*/) {}

GraceClock GraceClockNow() {
  GraceClock now;
  now.mono_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return now;
}

} // namespace pbr::os
