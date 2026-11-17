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

// iPad: a zero-width view in SDL's root view, from the keyboard layout guide's top edge down to the view's bottom edge.
// UIKit lays it out again when the docked keyboard's top moves, so its layoutSubviews is one change signal (the keyboard
// notifications are the others); the value itself is read from the guide (ScreenKeyboardHeight).
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

// The keyboard's final frame from the last notification, in the coordinates of the screen it is on (g_keyboard_screen);
// empty while it is hidden. iPhone places the keyboard with it; the iPad only falls back to it (ReadIpadKeyboard).
CGRect g_keyboard_frame = CGRectZero;
// The screen the keyboard notification names (iOS 16.1+: the notification's object). nil before the first notification
// or on older systems.
UIScreen* g_keyboard_screen = nil;
// iPad: the keyboard notifications seen since the shell last asked (TakeScreenKeyboardEvents), for its log line.
std::string g_events;

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

// SDL's root view: the view controller's view, where UIKit maintains a keyboard layout guide (11-17 device run: the
// UIWindow's own guide never moved while the keyboard was up).
UIView* RootView(UIWindow* window) {
  return window.rootViewController.view;
}

// iPad (iPadOS 27 windows): the keyboard notification's frame is in display coordinates while each window sits on a
// window-sized virtual screen, so it cannot be placed in our window (logged 2026-11-16: 129 / 19 pt instead of 484).
// The keyboard layout guide is in the view's own coordinates. An undocked or floating keyboard does not move it
// (followsUndockedKeyboard stays NO): nothing to lift for. The probe is created once, kept for the app's life and moved
// along with the root view.
UIKeyboardLayoutGuide* KeyboardGuide(UIView* view) {
  static PbrKeyboardGuideProbe* probe = nil;
  if (!probe) {
    probe = [[PbrKeyboardGuideProbe alloc] initWithFrame:CGRectZero];
    probe.userInteractionEnabled = NO;
    probe.translatesAutoresizingMaskIntoConstraints = NO;
  }
  UIKeyboardLayoutGuide* guide = view.keyboardLayoutGuide;
  if (probe.superview != view) {
    [probe removeFromSuperview];
    [view addSubview:probe];
    guide.followsUndockedKeyboard = NO;
    if (@available(iOS 17.0, *)) {
      // Hidden keyboard: the guide sits on the view's bottom edge, not on the home-indicator inset.
      guide.usesBottomSafeArea = NO;
    }
    [NSLayoutConstraint activateConstraints:@[
      [probe.leadingAnchor constraintEqualToAnchor:view.leadingAnchor],
      [probe.widthAnchor constraintEqualToConstant:0],
      [probe.topAnchor constraintEqualToAnchor:guide.topAnchor],
      [probe.bottomAnchor constraintEqualToAnchor:view.bottomAnchor],
    ]];
  }
  return guide;
}

bool SameSize(CGSize a, CGSize b) {
  return std::fabs(a.width - b.width) < 0.5 && std::fabs(a.height - b.height) < 0.5;
}

// What the iPad read last, for the log line (DescribeScreenKeyboard).
struct IpadKeyboardReading {
  const char* source = "none"; // guide | note | guide_hidden | none
  CGRect guide = CGRectZero;   // the guide's layoutFrame, in the root view
  CGSize view = CGSizeZero;    // the root view's bounds
  CGSize window = CGSizeZero;  // the UIWindow's bounds
  CGRect note = CGRectZero;    // the last notification's end frame, as given
  CGRect note_in_view = CGRectZero;
  CGRect note_scene = CGRectZero; // diagnostic: the same frame from the scene geometry's coordinate space (iOS 26)
  int cover = 0;
};
IpadKeyboardReading g_ipad;

// iPad: how far the docked keyboard reaches up into our window, in window points. The guide decides when it has a
// height; when it has none (hidden, undocked, or not maintained) while a notification says a keyboard is up, the
// notification's frame is placed in the view and counted only if it reaches the view's bottom edge.
int ReadIpadKeyboard(UIWindow* window) {
  IpadKeyboardReading r;
  UIView* view = RootView(window);
  r.window = window.bounds.size;
  r.note = g_keyboard_frame;
  if (!view) {
    g_ipad = r;
    return 0;
  }
  r.view = view.bounds.size;
  r.guide = KeyboardGuide(view).layoutFrame;
  const double window_h = window.bounds.size.height;
  if (!CGRectIsEmpty(g_keyboard_frame)) {
    UIScreen* screen = g_keyboard_screen ? g_keyboard_screen : window.windowScene.screen;
    if (screen) {
      r.note_in_view = [view convertRect:g_keyboard_frame fromCoordinateSpace:screen.coordinateSpace];
    }
    if (@available(iOS 26.0, *)) {
      if (window.windowScene) {
        r.note_scene = [view convertRect:g_keyboard_frame
                     fromCoordinateSpace:window.windowScene.effectiveGeometry.coordinateSpace];
      }
    }
  }
  // While SDL has resized its view away from the window (keyboard show / hide, UikitViewFillsWindow) the view's
  // coordinates say nothing about the window: keep the last value until the view is put back.
  if (!SameSize(r.view, r.window)) {
    r.source = g_ipad.source;
    r.cover = g_ipad.cover;
  } else if (r.guide.size.height > 0.5) {
    const CGRect in_window = [view convertRect:r.guide toView:window];
    r.source = "guide";
    r.cover = static_cast<int>(std::ceil(std::clamp(window_h - CGRectGetMinY(in_window), 0.0, window_h) - 0.001));
  } else if (!CGRectIsEmpty(r.note_in_view)) {
    r.source = "note";
    r.cover = KeyboardInsetInWindow(
        {r.note_in_view.origin.x, r.note_in_view.origin.y, r.note_in_view.size.width, r.note_in_view.size.height},
        r.view.width, r.view.height);
  } else {
    r.source = "guide_hidden";
  }
  g_ipad = r;
  return r.cover;
}

void AddEvent(const char* name, NSNotification* note) {
  NSNumber* local = note.userInfo[UIKeyboardIsLocalUserInfoKey];
  if (!g_events.empty()) {
    g_events += ',';
  }
  g_events += name;
  if (local && !local.boolValue) {
    g_events += "(other_app)";
  }
}

// iPad: every keyboard notification asks for a frame (and a log line), and again one main-loop turn later with the
// root view laid out, when the guide has taken the keyboard's new place.
void WatchIpadKeyboardNotifications(NSNotificationCenter* center) {
  const struct {
    NSNotificationName name;
    const char* label;
  } kNotes[] = {
      {UIKeyboardWillShowNotification, "will_show"},
      {UIKeyboardDidShowNotification, "did_show"},
      {UIKeyboardWillHideNotification, "will_hide"},
      {UIKeyboardDidHideNotification, "did_hide"},
      {UIKeyboardWillChangeFrameNotification, "will_change_frame"},
      {UIKeyboardDidChangeFrameNotification, "did_change_frame"},
  };
  for (const auto& entry : kNotes) {
    const char* label = entry.label;
    [center addObserverForName:entry.name
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* note) {
                      AddEvent(label, note);
                      NotifyChange();
                      dispatch_async(dispatch_get_main_queue(), ^{
                        UIWindow* window = AppWindow();
                        UIView* view = window ? RootView(window) : nil;
                        if (view && SameSize(view.bounds.size, window.bounds.size)) {
                          [view layoutIfNeeded];
                        }
                        AddEvent("later", nil);
                        NotifyChange();
                      });
                    }];
  }
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
      WatchIpadKeyboardNotifications(center);
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
    return ReadIpadKeyboard(window);
  }
  // iPhone: the window is the screen, so the notification's frame places the keyboard exactly (unchanged since 11-15).
  if (CGRectIsEmpty(g_keyboard_frame)) {
    return 0;
  }
  const CGRect in_window = KeyboardFrameInWindow(window);
  return KeyboardInsetInWindow({in_window.origin.x, in_window.origin.y, in_window.size.width, in_window.size.height},
                               window.bounds.size.width, window.bounds.size.height);
}

std::string TakeScreenKeyboardEvents() {
  return std::exchange(g_events, std::string());
}

std::string DescribeScreenKeyboard() {
  UIWindow* window = AppWindow();
  if (!window) {
    return "window=none";
  }
  if (IsIpadDevice()) {
    const IpadKeyboardReading& r = g_ipad;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "src=%s guide=%.0f,%.0f,%.0fx%.0f view=%.0fx%.0f window=%.0fx%.0f note=%.0f,%.0f,%.0fx%.0f "
                  "note_in_view=%.0f,%.0f,%.0fx%.0f note_scene=%.0f,%.0f,%.0fx%.0f",
                  r.source, r.guide.origin.x, r.guide.origin.y, r.guide.size.width, r.guide.size.height, r.view.width,
                  r.view.height, r.window.width, r.window.height, r.note.origin.x, r.note.origin.y, r.note.size.width,
                  r.note.size.height, r.note_in_view.origin.x, r.note_in_view.origin.y, r.note_in_view.size.width,
                  r.note_in_view.size.height, r.note_scene.origin.x, r.note_scene.origin.y, r.note_scene.size.width,
                  r.note_scene.size.height);
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
