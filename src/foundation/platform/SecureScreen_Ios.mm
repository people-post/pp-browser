#include "foundation/platform/SecureScreen.h"

#import <UIKit/UIKit.h>

namespace pbr::SecureScreen {

namespace {

// Main thread only (UIKit's; SDL's main thread on iOS).
bool g_secret = false;
bool g_inactive = false;
UIView* g_cover = nil;  // retained while shown

/** The app's window: the key one, else the first. */
UIWindow* AppWindow() {
  UIWindow* first = nil;
  for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
    if (![scene isKindOfClass:[UIWindowScene class]]) {
      continue;
    }
    for (UIWindow* window in ((UIWindowScene*)scene).windows) {
      if (window.isKeyWindow) {
        return window;
      }
      if (first == nil) {
        first = window;
      }
    }
  }
  return first;
}

/** The cover is up while a secret is on screen and the app is not active (the app switcher takes its picture). */
void Refresh() {
  const bool want = g_secret && g_inactive;
  if (want && g_cover == nil) {
    UIWindow* window = AppWindow();
    if (window == nil) {
      return;
    }
    UIView* cover = [[UIView alloc] initWithFrame:window.bounds];
    cover.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    cover.backgroundColor = [UIColor systemBackgroundColor];
    [window addSubview:cover];
    g_cover = cover;
  } else if (!want && g_cover != nil) {
    [g_cover removeFromSuperview];
    [g_cover release];
    g_cover = nil;
  }
}

void InstallObservers() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:UIApplicationWillResignActiveNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      g_inactive = true;
                      Refresh();
                    }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      g_inactive = false;
                      Refresh();
                    }];
  });
}

} // namespace

void SetSecretOnScreen(const bool on) {
  dispatch_block_t apply = ^{
    InstallObservers();
    g_secret = on;
    Refresh();
  };
  if ([NSThread isMainThread]) {
    apply();
  } else {
    dispatch_async(dispatch_get_main_queue(), apply);
  }
}

} // namespace pbr::SecureScreen
