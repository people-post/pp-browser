#include "foundation/platform/ScreenKeyboard.h"
#include "foundation/platform/ScreenKeyboardGeometry.h"

#import <UIKit/UIKit.h>

#include <utility>

namespace pbr {

namespace {

std::function<void()> g_on_change;
// The keyboard's final frame in screen coordinates; empty while it is hidden.
CGRect g_keyboard_frame = CGRectZero;

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

// Worked out on every call (the shell asks each frame), so a rotation or a window resize with the keyboard up is
// followed without waiting for another keyboard notification.
int ScreenKeyboardHeight() {
  if (CGRectIsEmpty(g_keyboard_frame)) {
    return 0;
  }
  UIWindow* window = AppWindow();
  if (!window) {
    return 0;
  }
  const CGRect in_window = [window convertRect:g_keyboard_frame fromCoordinateSpace:window.screen.coordinateSpace];
  return KeyboardInsetInWindow({in_window.origin.x, in_window.origin.y, in_window.size.width, in_window.size.height},
                               window.bounds.size.width, window.bounds.size.height);
}

} // namespace pbr
