#include "base/platform/Platform.h"

#if defined(__ANDROID__)
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pbr {

PlatformKind Platform::Detect() {
#if defined(__ANDROID__)
  return PlatformKind::Android;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  return PlatformKind::IOS;
#else
  return PlatformKind::Desktop;
#endif
}

bool Platform::IsMobile() {
  const PlatformKind kind = Detect();
  return kind == PlatformKind::Android || kind == PlatformKind::IOS;
}

bool Platform::IsDesktop() {
  return Detect() == PlatformKind::Desktop;
}

bool Platform::UsesPackagedAssets() {
  return Detect() != PlatformKind::Desktop;
}

bool Platform::SupportsSubprocessMcp() {
  return !IsMobile();
}

} // namespace pbr
