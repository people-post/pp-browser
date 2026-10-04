#include "foundation/platform/ScreenKeyboard.h"

#import <UIKit/UIKit.h>

#include <atomic>
#include <cmath>
#include <utility>

namespace pbr {

namespace {

std::atomic<int> g_height{0};
std::function<void()> g_on_change;

void SetHeight(const int height) {
  if (g_height.exchange(height) != height && g_on_change) {
    g_on_change();
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
                      const CGRect end = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
                      const CGRect screen = UIScreen.mainScreen.bounds;
                      const CGRect covered = CGRectIntersection(end, screen);
                      // A floating or undocked keyboard (iPad) does not sit on the bottom edge: nothing to lift for.
                      const bool docked = !CGRectIsEmpty(covered) && CGRectGetMaxY(covered) >= CGRectGetMaxY(screen) - 1.0;
                      SetHeight(docked ? static_cast<int>(std::ceil(covered.size.height)) : 0);
                    }];
    [center addObserverForName:UIKeyboardWillHideNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      SetHeight(0);
                    }];
  });
  return true;
}

int ScreenKeyboardHeight() {
  return g_height.load();
}

} // namespace pbr
