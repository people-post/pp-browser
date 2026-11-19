#include "foundation/platform/PlatformScreenBrightness.h"

#include "common/Logger.h"
#include "foundation/platform/ScreenBrightnessRestore.h"

#import <UIKit/UIKit.h>

namespace pbr {

namespace {

bool g_raised = false;
CGFloat g_before = 0;
constexpr CGFloat kRaisedBrightness = 1.0;

} // namespace

void SetScreenBrightnessMax(const bool raised) {
  @autoreleasepool {
    if (raised == g_raised) {
      return;
    }
    UIScreen* screen = UIScreen.mainScreen;
    if (raised) {
      g_before = screen.brightness;
      screen.brightness = kRaisedBrightness;
    } else if (ShouldRestoreScreenBrightness(static_cast<float>(screen.brightness),
                                             static_cast<float>(kRaisedBrightness))) {
      screen.brightness = g_before;
    } else {
      logging::getLogger("ScreenBrightness").info << "screen brightness changed meanwhile, left as the user set it";
    }
    g_raised = raised;
    logging::getLogger("ScreenBrightness").info << "screen brightness " << (raised ? "max" : "restored");
  }
}

} // namespace pbr
