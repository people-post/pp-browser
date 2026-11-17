#include "foundation/platform/ScreenKeyboard.h"
#include "foundation/platform/ScreenKeyboardGeometry.h"
#include "foundation/platform/UikitWindowGeometry.h"

#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace pbr {

namespace {

std::function<void()> g_on_change;

void NotifyChange() {
  if (g_on_change) {
    g_on_change();
  }
}

} // namespace

} // namespace pbr

// iPad: a zero-width view from the keyboard layout guide's top edge down to the window's bottom edge. UIKit lays it out
// again whenever the docked keyboard's top moves in our window (show, hide, rotation, window resize or move), so its
// layoutSubviews is the change signal; the value itself is read from the guide (ScreenKeyboardHeight).
@interface PbrKeyboardGuideProbe : UIView
@end

@implementation PbrKeyboardGuideProbe
- (void)layoutSubviews {
  [super layoutSubviews];
  pbr::NotifyChange();
}
@end

namespace pbr {

namespace {

// iPhone: the keyboard's final frame in the coordinates of the screen it is on (g_keyboard_screen); empty while it is
// hidden. The iPad reads the keyboard layout guide instead (KeyboardCoverFromGuide).
CGRect g_keyboard_frame = CGRectZero;
// The screen the keyboard notification names (iOS 16.1+: the notification's object). In an iPadOS window our UIWindow's
// `screen` is not that screen (logged 2026-11-16: its bounds are the window's own size, not mainScreen), so converting
// from it leaves the frame unconverted: off by the window's offset on the display, which in a landscape window can
// shrink the lift below the keyboard-size threshold. nil before the first notification or on older systems.
UIScreen* g_keyboard_screen = nil;

void RememberKeyboardScreen(id object) {
  UIScreen* screen = [object isKindOfClass:UIScreen.class] ? (UIScreen*)object : nil;
  if (screen == g_keyboard_screen) {
    return;
  }
#if !__has_feature(objc_arc)
  [screen retain];
  [g_keyboard_screen release];
#endif
  g_keyboard_screen = screen;
}

// The window our app draws in. On iPad it may be smaller than the screen and away from its bottom edge.
UIWindow* AppWindow() {
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:UIWindowScene.class]) {
      continue;
    }
    UIWindowScene* window_scene = (UIWindowScene*)scene;
    if (window_scene.keyWindow) {
      return window_scene.keyWindow;
    }
    for (UIWindow* window in window_scene.windows) {
      if (!window.hidden) {
        return window;
      }
    }
  }
  return nil;
}

// iPad (iPadOS 27 windows): the keyboard notification's frame is in display coordinates while each window sits on a
// window-sized virtual screen, so it cannot be placed in our window (logged 2026-11-16: 129 / 19 pt instead of 484).
// The keyboard layout guide is already in the window's own coordinates. An undocked or floating keyboard does not
// move it (followsUndockedKeyboard stays NO): nothing to lift for. Installed on first use and moved along with the
// window; the probe is created once and kept for the app's life.
UILayoutGuide* KeyboardGuide(UIWindow* window) {
  static PbrKeyboardGuideProbe* probe = nil;
  if (!probe) {
    probe = [[PbrKeyboardGuideProbe alloc] initWithFrame:CGRectZero];
    probe.userInteractionEnabled = NO;
    probe.translatesAutoresizingMaskIntoConstraints = NO;
  }
  UIKeyboardLayoutGuide* guide = window.keyboardLayoutGuide;
  if (probe.superview != window) {
    [probe removeFromSuperview];
    [window addSubview:probe];
    guide.followsUndockedKeyboard = NO;
    if (@available(iOS 17.0, *)) {
      // Hidden keyboard: the guide sits on the window's bottom edge, not on the home-indicator inset.
      guide.usesBottomSafeArea = NO;
    }
    [NSLayoutConstraint activateConstraints:@[
      [probe.leadingAnchor constraintEqualToAnchor:window.leadingAnchor],
      [probe.widthAnchor constraintEqualToConstant:0],
      [probe.topAnchor constraintEqualToAnchor:guide.topAnchor],
      [probe.bottomAnchor constraintEqualToAnchor:window.bottomAnchor],
    ]];
  }
  return guide;
}

// iPad: how far the docked keyboard reaches up into our window, in window points. An empty guide is a hidden keyboard
// (height 0 on the bottom edge) or one not laid out yet (all zero, which would read as covering the whole window).
int KeyboardCoverFromGuide(UIWindow* window) {
  const CGRect guide = KeyboardGuide(window).layoutFrame;
  if (CGRectIsEmpty(guide)) {
    return 0;
  }
  const double window_h = window.bounds.size.height;
  const double cover = window_h - CGRectGetMinY(guide);
  return static_cast<int>(std::ceil(std::clamp(cover, 0.0, window_h) - 0.001));
}

} // namespace

bool WatchScreenKeyboard(std::function<void()> on_change) {
  g_on_change = std::move(on_change);
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:UIKeyboardWillChangeFrameNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* note) {
                      // A keyboard shown for another app (split view, Stage Manager) says nothing about our window.
                      NSNumber* local = note.userInfo[UIKeyboardIsLocalUserInfoKey];
                      if (local && !local.boolValue) {
                        return;
                      }
                      g_keyboard_frame = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
                      RememberKeyboardScreen(note.object);
                      if (g_on_change) {
                        g_on_change();
                      }
                    }];
    [center addObserverForName:UIKeyboardWillHideNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      g_keyboard_frame = CGRectZero;
                      if (g_on_change) {
                        g_on_change();
                      }
                    }];
    if (IsIpadDevice()) {
      // The guide has its final place once the keyboard has moved; one more frame in case the probe was not laid out.
      [center addObserverForName:UIKeyboardDidChangeFrameNotification
                          object:nil
                           queue:[NSOperationQueue mainQueue]
                      usingBlock:^(NSNotification*) {
                        NotifyChange();
                      }];
    }
  });
  return true;
}

namespace {

// The keyboard frame in our window's coordinates, from the coordinate space of the screen the keyboard is on.
CGRect KeyboardFrameInWindow(UIWindow* window) {
  UIScreen* screen = g_keyboard_screen ? g_keyboard_screen : window.screen;
  return [window convertRect:g_keyboard_frame fromCoordinateSpace:screen.coordinateSpace];
}

} // namespace

// Worked out on every call (the shell asks each frame), so a rotation, a window resize or a window move with the
// keyboard up is followed without waiting for another keyboard notification.
int ScreenKeyboardHeight() {
  UIWindow* window = AppWindow();
  if (!window) {
    return 0;
  }
  if (IsIpadDevice()) {
    return KeyboardCoverFromGuide(window);
  }
  // iPhone: the window is the screen, so the notification's frame places the keyboard exactly (unchanged since 11-15).
  if (CGRectIsEmpty(g_keyboard_frame)) {
    return 0;
  }
  const CGRect in_window = KeyboardFrameInWindow(window);
  return KeyboardInsetInWindow({in_window.origin.x, in_window.origin.y, in_window.size.width, in_window.size.height},
                               window.bounds.size.width, window.bounds.size.height);
}

std::string DescribeScreenKeyboard() {
  UIWindow* window = AppWindow();
  if (!window) {
    return "window=none";
  }
  if (IsIpadDevice()) {
    const CGRect guide = KeyboardGuide(window).layoutFrame;
    char line[160];
    std::snprintf(line, sizeof(line), "src=guide guide=%.0f,%.0f,%.0fx%.0f window=%.0fx%.0f", guide.origin.x,
                  guide.origin.y, guide.size.width, guide.size.height, window.bounds.size.width,
                  window.bounds.size.height);
    return line;
  }
  const CGRect raw = g_keyboard_frame;
  const CGRect in_window = KeyboardFrameInWindow(window);
  char line[256];
  std::snprintf(line, sizeof(line),
                "kb_raw=%.0f,%.0f,%.0fx%.0f kb_in_window=%.0f,%.0f,%.0fx%.0f window=%.0fx%.0f kb_screen=%s",
                raw.origin.x, raw.origin.y, raw.size.width, raw.size.height, in_window.origin.x, in_window.origin.y,
                in_window.size.width, in_window.size.height, window.bounds.size.width, window.bounds.size.height,
                !g_keyboard_screen                         ? "none"
                : g_keyboard_screen == window.screen       ? "window"
                : g_keyboard_screen == UIScreen.mainScreen ? "main"
                                                           : "other");
  return line;
}

} // namespace pbr
