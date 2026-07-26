#if defined(__APPLE__)

#include "base/platform/desktop/WindowChrome_Darwin.h"

#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <SDL3/SDL.h>

namespace pbr {
namespace desktop {
namespace {

/** Standard macOS window corner radius (Big Sur+). */
constexpr CGFloat kWindowCornerRadiusPt = 10.0;

} // namespace

void ApplyMacWindowRoundedCorners(SDL_Window* window, bool square_corners) {
  if (!window) {
    return;
  }
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  void* cocoa = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
  if (!cocoa) {
    return;
  }

  NSWindow* nswindow = (__bridge NSWindow*)cocoa;
  NSView* content = [nswindow contentView];
  if (!content) {
    return;
  }

  const CGFloat radius = square_corners ? 0.0 : kWindowCornerRadiusPt;

  // Transparent outside the rounded clip so the desktop shows through.
  // Requires SDL_WINDOW_TRANSPARENT at create time (NSOpenGLCPSurfaceOpacity=0,
  // clear layer fill); otherwise WindowServer treats the full rect as opaque black.
  [nswindow setOpaque:NO];
  [nswindow setBackgroundColor:[NSColor clearColor]];
  // SDL disables the shadow for transparent windows; restore a system-like drop shadow.
  [nswindow setHasShadow:YES];

  content.wantsLayer = YES;
  CALayer* layer = content.layer;
  if (!layer) {
    return;
  }
  layer.opaque = NO;
  layer.backgroundColor = CGColorGetConstantColor(kCGColorClear);
  layer.cornerRadius = radius;
  layer.masksToBounds = YES;
  if (@available(macOS 10.13, *)) {
    layer.maskedCorners = kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner | kCALayerMinXMaxYCorner |
                          kCALayerMaxXMaxYCorner;
  }
  [nswindow invalidateShadow];
}

} // namespace desktop
} // namespace pbr

#endif // !TARGET_OS_IPHONE
#endif // __APPLE__
