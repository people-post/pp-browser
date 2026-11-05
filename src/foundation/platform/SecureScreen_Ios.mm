#include "foundation/platform/SecureScreen.h"

#import <UIKit/UIKit.h>

namespace pbr::SecureScreen {

namespace {

// Main thread only (UIKit's; SDL's main thread on iOS).
bool g_secret = false;
bool g_inactive = false;
/** A secret can be read on screen (SetSecretLegible), and the screen is being recorded or mirrored. */
bool g_legible = false;
bool g_captured = false;
UIView* g_cover = nil;  // retained while shown
/** The cover says why only while it hides a recording (the app switcher's picture needs no words). */
UILabel* g_cover_label = nil;  // owned by g_cover

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

bool ScreenCaptured() {
  return [UIScreen mainScreen].isCaptured ? true : false;
}

/**
 * The cover is up while a secret is on screen and the app is not active (the app switcher takes its picture), or while
 * a secret can be read and the screen is recorded or mirrored (iOS cannot keep those from copying it).
 */
void Refresh() {
  const bool for_capture = g_legible && g_captured;
  const bool want = (g_secret && g_inactive) || for_capture;
  if (want && g_cover == nil) {
    UIWindow* window = AppWindow();
    if (window == nil) {
      return;
    }
    UIView* cover = [[UIView alloc] initWithFrame:window.bounds];
    cover.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    cover.backgroundColor = [UIColor systemBackgroundColor];
    UILabel* label = [[UILabel alloc] initWithFrame:CGRectInset(cover.bounds, 32, 0)];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.textColor = [UIColor secondaryLabelColor];
    label.text = [[NSBundle mainBundle] localizedStringForKey:@"SECRET_HIDDEN_WHILE_CAPTURED"
                                                        value:@"Hidden while your screen is recorded or shared"
                                                        table:nil];
    [cover addSubview:label];
    g_cover_label = label;
    [label release];  // the cover holds it
    [window addSubview:cover];
    g_cover = cover;
  } else if (!want && g_cover != nil) {
    [g_cover removeFromSuperview];
    [g_cover release];
    g_cover = nil;
    g_cover_label = nil;
  }
  if (g_cover_label != nil) {
    g_cover_label.hidden = for_capture ? NO : YES;
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
    [center addObserverForName:UIScreenCapturedDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      g_captured = ScreenCaptured();
                      Refresh();
                    }];
  });
}

void OnMain(dispatch_block_t apply) {
  if ([NSThread isMainThread]) {
    apply();
  } else {
    dispatch_async(dispatch_get_main_queue(), apply);
  }
}

} // namespace

void SetSecretOnScreen(const bool on) {
  OnMain(^{
    InstallObservers();
    g_secret = on;
    Refresh();
  });
}

void SetSecretLegible(const bool on) {
  OnMain(^{
    InstallObservers();
    g_legible = on;
    g_captured = ScreenCaptured();
    Refresh();
  });
}

} // namespace pbr::SecureScreen
