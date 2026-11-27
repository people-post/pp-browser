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
 * than the system's 10pt. This one layer owns the whole edge: it clips the content and draws the hairline outline, so
 * there is one curve, not two. `.shell-body--framed.shell-body--round` in components.rcss therefore draws no border of
 * its own on the Mac; change the radius here only (the one-line change).
 */
constexpr CGFloat kWindowCornerRadiusPt = 18.0;

/**
 * The outline colour, the same as the shell's `.shell-body--framed` border in colors-light.rcss / colors-dark.rcss
 * (`#3c3c434a` / `#54545899`): keep them equal (a test compares the numbers).
 */
constexpr CGFloat kOutlineLightRgb[3] = {0x3c / 255.0, 0x3c / 255.0, 0x43 / 255.0};
constexpr CGFloat kOutlineLightAlpha = 0x4a / 255.0;
constexpr CGFloat kOutlineDarkRgb[3] = {0x54 / 255.0, 0x54 / 255.0, 0x58 / 255.0};
constexpr CGFloat kOutlineDarkAlpha = 0x99 / 255.0;

} // namespace

void ApplyMacWindowRoundedCorners(SDL_Window* window, bool square_corners, bool dark) {
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
  if (@available(macOS 10.15, *)) {
    layer.cornerCurve = kCACornerCurveContinuous; // the system windows' corner shape
  }

  // The hairline outline, on the same layer as the clip. One device pixel wide (0.5pt on a Retina screen); none when the
  // window is square (maximized / full screen). Re-applied when the theme, the screen scale or the window state changes.
  const CGFloat scale = nswindow.backingScaleFactor > 0.0 ? nswindow.backingScaleFactor : 1.0;
  layer.borderWidth = square_corners ? 0.0 : 1.0 / scale;
  const CGFloat* rgb = dark ? kOutlineDarkRgb : kOutlineLightRgb;
  NSColor* outline = [NSColor colorWithSRGBRed:rgb[0]
                                         green:rgb[1]
                                          blue:rgb[2]
                                         alpha:(dark ? kOutlineDarkAlpha : kOutlineLightAlpha)];
  layer.borderColor = outline.CGColor;
  [nswindow invalidateShadow];
}

} // namespace desktop
} // namespace pbr

#endif // !TARGET_OS_IPHONE
#endif // __APPLE__
