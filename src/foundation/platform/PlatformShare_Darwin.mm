#include "foundation/platform/PlatformShare.h"

#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

#include <string>

// The picker drives its menu asynchronously; AppKit tells the delegate when the user picked a
// service or dismissed the menu, and only then may the picker go (releasing it earlier, e.g. on the
// next share, is a use-after-free). Objective-C++ without ARC here.
@interface PpSharePickerDelegate : NSObject <NSSharingServicePickerDelegate>
@end

@implementation PpSharePickerDelegate
- (void)sharingServicePicker:(NSSharingServicePicker*)picker didChooseSharingService:(NSSharingService*)service {
  (void)service;
  dispatch_async(dispatch_get_main_queue(), ^{
    [picker release]; // balances the alloc in PlatformShareText; the block's own retain is separate
  });
}
@end

namespace pbr {

bool PlatformShareText(SDL_Window* window, const std::string& text, const ShareAnchor anchor) {
  if (window == nullptr || text.empty()) {
    return false;
  }
  @autoreleasepool {
    NSWindow* nswindow = static_cast<NSWindow*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
    NSView* content = nswindow.contentView;
    if (content == nil) {
      return false;
    }
    static PpSharePickerDelegate* delegate = [[PpSharePickerDelegate alloc] init]; // lives for the process
    NSString* item = [NSString stringWithUTF8String:text.c_str()];
    NSSharingServicePicker* picker = [[NSSharingServicePicker alloc] initWithItems:@[ item ]]; // released by the delegate
    picker.delegate = delegate;
    // RmlUi gives window pixels from the top-left; the view wants points from the bottom-left.
    const NSRect bounds = content.bounds;
    const float density = SDL_GetWindowPixelDensity(window) > 0.f ? SDL_GetWindowPixelDensity(window) : 1.f;
    NSRect rect = NSMakeRect(NSMidX(bounds), NSMidY(bounds), 1, 1);
    if (anchor.w > 0.f && anchor.h > 0.f) {
      rect = NSMakeRect(anchor.x / density, NSHeight(bounds) - (anchor.y + anchor.h) / density, anchor.w / density,
                        anchor.h / density);
    }
    [picker showRelativeToRect:rect ofView:content preferredEdge:NSMinYEdge];
    return true;
  }
}

} // namespace pbr
