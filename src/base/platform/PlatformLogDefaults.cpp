#include "base/platform/PlatformLogDefaults.h"

#include "base/platform/Platform.h"

namespace pbr {

logging::Level DefaultRootLogLevel(bool debug_mode) {
  if (Platform::Detect() == PlatformKind::IOS) {
    return debug_mode ? logging::kLevelDebug : logging::Level::INFO;
  }
  return debug_mode ? logging::kLevelDebug : logging::Level::WARNING;
}

} // namespace pbr
