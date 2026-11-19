#include "gui/QrBrightness.h"

#include "foundation/runtime/AppLifecycle.h"
#include "foundation/platform/PlatformLifecycleEvents.h"
#include "foundation/platform/PlatformScreenBrightness.h"

namespace pbr {

QrBrightnessHold& QrBrightness() {
  static QrBrightnessHold hold(SetScreenBrightnessMax, [] { return AppLifecycle::IsVisible(); });
  static const bool listening = [] {
    // Both edges re-read AppLifecycle::IsVisible(): out restores at once, back raises again if the page is still up.
    PlatformLifecycleEvents::AddEnteredBackgroundListener([] { QrBrightness().Refresh(); });
    PlatformLifecycleEvents::AddDidEnterForegroundListener([] { QrBrightness().Refresh(); });
    return true;
  }();
  (void)listening;
  return hold;
}

} // namespace pbr
