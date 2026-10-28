#include "foundation/platform/PlatformLogDefaults.h"

#include "foundation/platform/Platform.h"
#include "common/PbrCompat.h"

namespace pbr {

logging::Level DefaultRootLogLevel(bool debug_mode) {
  // INFO everywhere: the log file (and the diagnostics a user can send from it) needs the INFO lines, and the
  // root logger's level is the only gate in front of both the file and the console handler. Android release
  // promotes emitted priority to WARNING via DefaultEmitFloor for logcat :W dogfood.
  return debug_mode ? logging::kLevelDebug : logging::Level::INFO;
}

logging::Level DefaultEmitFloor(bool debug_mode) {
  if (Platform::Detect() == PlatformKind::Android && !debug_mode) {
    return logging::Level::WARNING;
  }
  return logging::kLevelDebug;
}

} // namespace pbr
