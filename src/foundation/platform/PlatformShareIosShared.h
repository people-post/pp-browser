#pragma once
// Internal to the iOS share sheets (PlatformShareImage_Ios.mm, PlatformShareFile_Ios.mm): the controller to present
// from and the iPad popover's anchor. Definitions are in PlatformShareImage_Ios.mm. Objective-C++ only.

#include "foundation/platform/PlatformShare.h"

#import <UIKit/UIKit.h>

struct SDL_Window;

namespace pbr::ios_share {

/** The top-most controller presented in `window`, or nil when there is none or UIKit is mid-transition. */
UIViewController* SheetHost(SDL_Window* window);

/** On an iPad the sheet is a popover: points it at `anchor` (window pixels), else at the middle of `host`'s view. */
void AnchorSheet(UIActivityViewController* sheet, UIViewController* host, SDL_Window* window, ShareAnchor anchor);

} // namespace pbr::ios_share
