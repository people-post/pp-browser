#pragma once

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <functional>

namespace pbr {

enum class AppLifecycleState { Foreground, Background };

class AppLifecycle {
public:
  /** A listener's handle; RemoveListener takes it back (each owner removes only its own). */
  using ListenerId = uint64_t;
  /** Bind `Runtime.Lifecycle`. Idempotent. Called from AppRuntime::Initialize. */
  static void InitLogging();
  static logging::Logger& logger();

  static AppLifecycleState Current();
  static bool IsForeground();

  /**
   * True when the user is actively attending to the app UI.
   * Desktop: input-focused and not minimized (clicking another app counts as inattentive).
   * Mobile: same as IsForeground() (desktop focus/minimize hooks are unused).
   */
  static bool IsUserAttentive();

  /** UI/SDL thread only — tracks desktop focus independent of minimize/lifecycle. */
  static void SetDesktopInputFocused(bool focused);
  static void SetDesktopMinimized(bool minimized);
  /**
   * A desktop window minimized / restored. Minimized is not attentive (alerts notify) but the app
   * keeps working: calls ring and messages arrive, as in any desktop messenger. Background (paused
   * work) is the mobile OS's state, not a window's.
   */
  static void OnDesktopMinimized();
  static void OnDesktopRestored();

  /**
   * P011 — stay connected in the background (opt-in): entering the background then keeps the
   * app's work running (it still reads not attentive, so alerts notify). Listeners are told on a
   * change (the Android foreground service follows it).
   */
  static void SetStayConnected(bool on);
  static bool StaysConnected();
  static ListenerId AddStayConnectedListener(std::function<void(bool)> listener);

  static void OnWillEnterBackground();
  static void OnDidEnterForeground();
  static void OnLowMemory();

  static ListenerId AddBackgroundListener(std::function<void()> listener);
  static ListenerId AddForegroundListener(std::function<void()> listener);
  static void RemoveListener(ListenerId id);
};

} // namespace pbr
