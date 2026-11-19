// iOS / iPadOS: a received attachment through the system share sheet (Save to Files, Save Image, another app). The
// sheet gets the file's URL; the app the user picks makes its own copy. Nothing is deleted here.
#include "foundation/platform/PlatformShareFile.h"
#include "foundation/platform/PlatformShareIosShared.h"

#import <UIKit/UIKit.h>

namespace pbr {

bool PlatformCanShareFile() {
  return true;
}

// Objective-C++ without ARC here (as in PlatformShareImage_Ios.mm): autorelease what we allocate.
bool PlatformShareFile(SDL_Window* window, const std::string& path, const std::string& /*mime*/,
                       const ShareAnchor anchor) {
  if (window == nullptr || path.empty()) {
    return false;
  }
  @autoreleasepool {
    NSString* file = [NSString stringWithUTF8String:path.c_str()];
    if (file == nil || ![[NSFileManager defaultManager] fileExistsAtPath:file]) {
      return false;
    }
    UIViewController* host = ios_share::SheetHost(window);
    if (host == nil) {
      return false;
    }
    UIActivityViewController* sheet =
        [[[UIActivityViewController alloc] initWithActivityItems:@[ [NSURL fileURLWithPath:file] ]
                                           applicationActivities:nil] autorelease];
    ios_share::AnchorSheet(sheet, host, window, anchor);
    [host presentViewController:sheet animated:YES completion:nil];
    return true;
  }
}

} // namespace pbr
