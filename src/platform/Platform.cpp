#include "platform/Platform.h"

namespace pbr {

PlatformKind Platform::Detect() {
  return PlatformKind::Desktop;
}

} // namespace pbr
