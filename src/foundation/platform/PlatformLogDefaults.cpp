#include "foundation/platform/PlatformLogDefaults.h"

#include "foundation/platform/Platform.h"
#include "common/PbrCompat.h"

namespace pbr {

logging::Level DefaultRootLogLevel(bool debug_mode) {
  if (Platform::Detect() == PlatformKind::IOS) {
    return debug_mode ? logging::kLevelDebug : logging::Level::INFO;
  }
  // Android release uses INFO so call-site info passes the filter; DefaultEmitFloor
  // then promotes emitted priority to WARNING for logcat :W dogfood.
  if (Platform::Detect() == PlatformKind::Android) {
    return debug_mode ? logging::kLevelDebug : logging::Level::INFO;
  }
  return debug_mode ? logging::kLevelDebug : logging::Level::WARNING;
}

logging::Level DefaultEmitFloor(bool debug_mode) {
  if (Platform::Detect() == PlatformKind::Android && !debug_mode) {
    return logging::Level::WARNING;
  }
  return logging::kLevelDebug;
}

} // namespace pbr
