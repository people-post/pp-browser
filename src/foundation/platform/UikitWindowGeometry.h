#pragma once

#include <cmath>
#include <string>

struct SDL_Window;

namespace pbr {

/** What UIKit says about SDL's window, in points (sizes) and points-to-pixels factors (scales). */
struct UikitViewGeometry {
  double window_w = 0;     // the UIWindow's bounds
  double window_h = 0;
  double view_w = 0;       // SDL's root view (the GL view) bounds
  double view_h = 0;
  double view_scale = 0;   // the view's contentScaleFactor: the GL drawable is view bounds x this
  double screen_scale = 0; // the window's screen nativeScale: SDL reports pixels as view bounds x this
};

struct UikitGeometryFix {
  bool resize_view = false;  // the view does not fill its window
  bool rescale_view = false; // the drawable's scale differs from the one SDL reports pixels in
  bool Any() const { return resize_view || rescale_view; }
};

/**
 * What to correct so the GL drawable, SDL's pixel size and the RmlUi context agree with the window: on iPadOS 27 the
 * window can be resized and moved to another screen (TN3192), and SDL only follows a layout pass of its view.
 * Nothing while UIKit has not given the window a size yet.
 */
inline UikitGeometryFix PlanUikitGeometryFix(const UikitViewGeometry& g) {
  UikitGeometryFix fix;
  if (g.window_w <= 0 || g.window_h <= 0) {
    return fix;
  }
  fix.resize_view = std::fabs(g.view_w - g.window_w) > 0.5 || std::fabs(g.view_h - g.window_h) > 0.5;
  fix.rescale_view = g.screen_scale > 0 && std::fabs(g.view_scale - g.screen_scale) > 0.001;
  return fix;
}

/**
 * iOS: makes SDL's view fill its window and its drawable use the scale SDL reports pixels in, then lays it out so
 * SDL sends the resize events (RESIZED, PIXEL_SIZE_CHANGED) that resync the context. Returns what it corrected;
 * nothing elsewhere. Call on the UI thread, outside a frame.
 */
UikitGeometryFix SyncUikitWindowGeometry(SDL_Window* window);

/**
 * iOS 26+: how far down (window points) content must start to stay clear of the window controls (close / minimise /
 * full screen) in the top-left corner when the app runs in a window; at least the plain top safe area. 0 elsewhere.
 */
int WindowControlsTopInset(SDL_Window* window);

/** iOS: one line with the scene / window / view sizes, scales and interface orientation, for the log. "" elsewhere. */
std::string DescribeUikitWindowGeometry(SDL_Window* window);

} // namespace pbr
