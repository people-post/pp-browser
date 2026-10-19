#include "foundation/platform/PlatformShare.h"

#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>

#include <string>

namespace pbr {

// Objective-C++ without ARC here: autorelease what we allocate.
bool PlatformShareText(SDL_Window* window, const std::string& text, const ShareAnchor anchor) {
  if (window == nullptr || text.empty()) {
    return false;
  }
  @autoreleasepool {
    UIWindow* uiwindow = static_cast<UIWindow*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr));
    UIViewController* host = uiwindow.rootViewController;
    if (host == nil) {
      return false;
    }
    while (host.presentedViewController != nil) {
      host = host.presentedViewController;
    }
    if (host.isBeingPresented || host.isBeingDismissed || host.transitionCoordinator != nil) {
      return false; // UIKit refuses a second presentation mid-transition without telling us
    }
    NSString* item = [NSString stringWithUTF8String:text.c_str()];
    UIActivityViewController* sheet =
        [[[UIActivityViewController alloc] initWithActivityItems:@[ item ] applicationActivities:nil] autorelease];
    // iPad presents this as a popover and needs an anchor: the button (window pixels → points) or the centre.
    if (sheet.popoverPresentationController != nil) {
      const float density = SDL_GetWindowPixelDensity(window) > 0.f ? SDL_GetWindowPixelDensity(window) : 1.f;
      CGRect rect = CGRectMake(CGRectGetMidX(host.view.bounds), CGRectGetMidY(host.view.bounds), 1, 1);
      if (anchor.w > 0.f && anchor.h > 0.f) {
        rect = CGRectMake(anchor.x / density, anchor.y / density, anchor.w / density, anchor.h / density);
      }
      sheet.popoverPresentationController.sourceView = host.view;
      sheet.popoverPresentationController.sourceRect = rect;
      sheet.popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [host presentViewController:sheet animated:YES completion:nil];
    return true;
  }
}

} // namespace pbr
