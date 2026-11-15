#include "foundation/platform/UikitWindowGeometry.h"

#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

namespace pbr {

namespace {

UIWindow* WindowOf(SDL_Window* window) {
  if (!window) {
    return nil;
  }
  void* ptr = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
  return (__bridge UIWindow*)ptr;
}

// SDL's root view: the GL view once the context exists.
UIView* ViewOf(UIWindow* uiwindow) {
  return uiwindow.rootViewController.view;
}

const char* OrientationName(UIInterfaceOrientation o) {
  switch (o) {
    case UIInterfaceOrientationPortrait:
      return "portrait";
    case UIInterfaceOrientationPortraitUpsideDown:
      return "portrait_down";
    case UIInterfaceOrientationLandscapeLeft:
      return "landscape_left";
    case UIInterfaceOrientationLandscapeRight:
      return "landscape_right";
    default:
      return "unknown";
  }
}

} // namespace

UikitGeometryFix SyncUikitWindowGeometry(SDL_Window* window) {
  UIWindow* uiwindow = WindowOf(window);
  UIView* view = ViewOf(uiwindow);
  if (!uiwindow || !view) {
    return {};
  }
  UikitViewGeometry g;
  g.window_w = uiwindow.bounds.size.width;
  g.window_h = uiwindow.bounds.size.height;
  g.view_w = view.bounds.size.width;
  g.view_h = view.bounds.size.height;
  g.view_scale = view.contentScaleFactor;
  g.screen_scale = uiwindow.screen.nativeScale;
  const UikitGeometryFix fix = PlanUikitGeometryFix(g);
  if (!fix.Any()) {
    return fix;
  }
  if (fix.rescale_view) {
    view.contentScaleFactor = uiwindow.screen.nativeScale;
  }
  if (fix.resize_view) {
    view.frame = uiwindow.bounds;
  }
  // The GL view resizes its drawable in layoutSubviews; SDL's view controller then sends RESIZED.
  [view setNeedsLayout];
  [view layoutIfNeeded];
  return fix;
}

int WindowControlsTopInset(SDL_Window* window) {
  UIView* view = ViewOf(WindowOf(window));
  if (!view) {
    return 0;
  }
  if (@available(iOS 26.0, *)) {
    // Vertical corner adaptation: the top inset grows to clear the window controls in the top-left corner.
    // Read every frame (ShellHost's safe area): build the region once.
    static UIViewLayoutRegion* region =
        [UIViewLayoutRegion safeAreaLayoutRegionWithCornerAdaptation:UIViewLayoutRegionAdaptivityAxisVertical];
    const UIEdgeInsets insets = [view edgeInsetsForLayoutRegion:region];
    return static_cast<int>(std::ceil(insets.top));
  }
  return 0;
}

std::string DescribeUikitWindowGeometry(SDL_Window* window) {
  UIWindow* uiwindow = WindowOf(window);
  UIView* view = ViewOf(uiwindow);
  if (!uiwindow || !view) {
    return {};
  }
  UIWindowScene* scene = uiwindow.windowScene;
  const CGRect scene_bounds = scene ? scene.coordinateSpace.bounds : CGRectZero;
  UIInterfaceOrientation orientation = UIInterfaceOrientationUnknown;
  if (@available(iOS 16.0, *)) {
    orientation = scene ? scene.effectiveGeometry.interfaceOrientation : UIInterfaceOrientationUnknown;
  }
  UIScreen* screen = uiwindow.screen;
  char line[512];
  std::snprintf(line, sizeof(line),
                "scene=%.0fx%.0f window=%.0fx%.0f view=%.0fx%.0f view_scale=%.3f screen=%.0fx%.0f native_scale=%.3f "
                "main_screen=%d orientation=%s safe_top=%.0f controls_top=%d",
                scene_bounds.size.width, scene_bounds.size.height, uiwindow.bounds.size.width,
                uiwindow.bounds.size.height, view.bounds.size.width, view.bounds.size.height, view.contentScaleFactor,
                screen.bounds.size.width, screen.bounds.size.height, screen.nativeScale,
                screen == UIScreen.mainScreen ? 1 : 0, OrientationName(orientation), view.safeAreaInsets.top,
                WindowControlsTopInset(window));
  return line;
}

} // namespace pbr
