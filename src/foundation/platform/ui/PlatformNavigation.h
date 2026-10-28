#pragma once

#include <functional>

namespace pbr {

class PlatformNavigation {
public:
  static void SetDismissHandler(std::function<bool()> handler);
  /**
   * The Escape key only (not the system Back): asked first. 0 = not for the composer, 1 = the composer used it,
   * 2 = composer focused with nothing to do (Escape then dismisses as usual but never quits the app).
   */
  static void SetEscapeKeyHandler(std::function<int()> handler);
  static bool OnDismissKey();
  static bool OnSystemBack();
};

} // namespace pbr
