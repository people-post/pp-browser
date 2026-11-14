#include "foundation/platform/PlatformShareImage.h"

#include "foundation/platform/NativeFileDialog.h"

#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

// Same delegate rule as PlatformShare_Darwin.mm: the picker may only go once AppKit says the user is done with it.
// Objective-C++ without ARC here.
@interface PpShareImagePickerDelegate : NSObject <NSSharingServicePickerDelegate>
@end

@implementation PpShareImagePickerDelegate
- (void)sharingServicePicker:(NSSharingServicePicker*)picker didChooseSharingService:(NSSharingService*)service {
  (void)service;
  dispatch_async(dispatch_get_main_queue(), ^{
    [picker release]; // balances the alloc in PlatformShareImage
  });
}
@end

namespace pbr {

bool PlatformCanShareImage() {
  return true;
}

bool PlatformShareImage(SDL_Window* window, const std::vector<uint8_t>& png, const ShareAnchor anchor) {
  if (window == nullptr || png.empty()) {
    return false;
  }
  @autoreleasepool {
    NSWindow* nswindow = static_cast<NSWindow*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
    NSView* content = nswindow.contentView;
    if (content == nil) {
      return false;
    }
    NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
    NSImage* image = [[[NSImage alloc] initWithData:data] autorelease];
    if (image == nil) {
      return false;
    }
    static PpShareImagePickerDelegate* delegate = [[PpShareImagePickerDelegate alloc] init]; // lives for the process
    NSSharingServicePicker* picker =
        [[NSSharingServicePicker alloc] initWithItems:@[ image ]]; // released by the delegate
    picker.delegate = delegate;
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

ImageSaveKind PlatformImageSaveKind() {
  return ImageSaveKind::File;
}

void PlatformSaveImage(SDL_Window* window, const std::string& suggested_name, std::vector<uint8_t> png,
                       std::function<void(ImageSaveResult)> done) {
  if (!done) {
    return;
  }
  if (png.empty()) {
    done(ImageSaveResult::Failed);
    return;
  }
  // SaveFileAs answers false for a cancel and for a failed write alike: neither is worth a message here.
  SaveFileAs(window, suggested_name, std::string(png.begin(), png.end()), [done = std::move(done)](const bool saved) {
    done(saved ? ImageSaveResult::Saved : ImageSaveResult::Cancelled);
  });
}

} // namespace pbr
