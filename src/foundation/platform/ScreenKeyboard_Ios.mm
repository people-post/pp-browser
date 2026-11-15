#include "foundation/platform/ScreenKeyboard.h"
#include "foundation/platform/ScreenKeyboardGeometry.h"

#import <UIKit/UIKit.h>

#include <cstdio>
#include <utility>

namespace pbr {

namespace {

std::function<void()> g_on_change;
// The keyboard's final frame in the coordinates of the screen it is on (g_keyboard_screen); empty while it is hidden.
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
  if (CGRectIsEmpty(g_keyboard_frame)) {
    return 0;
  }
  UIWindow* window = AppWindow();
  if (!window) {
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
