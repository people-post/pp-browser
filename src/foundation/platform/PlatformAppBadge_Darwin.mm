// macOS: the red number on PP's Dock icon.
#include "foundation/platform/PlatformAppBadge.h"

#import <AppKit/AppKit.h>

namespace pbr {

void SetAppBadgeCount(int count) {
  const int shown = count > 0 ? count : 0;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp dockTile].badgeLabel = shown > 0 ? [NSString stringWithFormat:@"%d", shown] : nil;
  });
}

} // namespace pbr
