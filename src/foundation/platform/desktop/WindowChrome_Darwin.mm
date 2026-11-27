#if defined(__APPLE__)

#include "foundation/platform/desktop/WindowChrome_Darwin.h"

#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <SDL3/SDL.h>

namespace pbr {
namespace desktop {
namespace {

/**
 * The window's outer corner radius: ours (the window is borderless and transparent; this layer mask is its shape), softer
 * than the system's 10pt. `.shell-body--framed.shell-body--round` in components.rcss draws the 1dp outline with the same
 * radius in dp (equal to pt here): change both together.
 */
constexpr CGFloat kWindowCornerRadiusPt = 18.0;

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
    layer.maskedCorners =
        kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner | kCALayerMinXMaxYCorner | kCALayerMaxXMaxYCorner;
  }
  [nswindow invalidateShadow];
}

} // namespace desktop
} // namespace pbr

#endif // !TARGET_OS_IPHONE
#endif // __APPLE__
