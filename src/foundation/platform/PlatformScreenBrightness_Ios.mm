#include "foundation/platform/PlatformScreenBrightness.h"

#include "common/Logger.h"

#import <UIKit/UIKit.h>

namespace pbr {

namespace {

bool g_raised = false;
CGFloat g_before = 0;

} // namespace

void SetScreenBrightnessMax(const bool raised) {
  @autoreleasepool {
    if (raised == g_raised) {
      return;
    }
    UIScreen* screen = UIScreen.mainScreen;
    if (raised) {
      g_before = screen.brightness;
      screen.brightness = 1.0;
    } else {
      screen.brightness = g_before;
    }
    g_raised = raised;
    logging::getLogger("ScreenBrightness").info << "screen brightness " << (raised ? "max" : "restored");
  }
}

} // namespace pbr
