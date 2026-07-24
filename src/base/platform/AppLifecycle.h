#pragma once

#include <functional>

namespace pbr {

enum class AppLifecycleState { Foreground, Background };

class AppLifecycle {
public:
  static AppLifecycleState Current();
  static bool IsForeground();

  /**
   * True when the user is actively attending to the app UI.
   * Desktop: input-focused and not minimized (clicking another app counts as inattentive).
   * Mobile: same as IsForeground().
   */
  static bool IsUserAttentive();

  /** UI/SDL thread only — tracks desktop focus independent of minimize/lifecycle. */
  static void SetDesktopInputFocused(bool focused);
  static void SetDesktopMinimized(bool minimized);

  static void OnWillEnterBackground();
  static void OnDidEnterForeground();
  static void OnLowMemory();

  static void AddBackgroundListener(std::function<void()> listener);
  static void AddForegroundListener(std::function<void()> listener);
  static void ClearBackgroundListeners();
  static void ClearForegroundListeners();
};

} // namespace pbr
