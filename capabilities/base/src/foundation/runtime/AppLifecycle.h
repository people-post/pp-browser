#pragma once

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <chrono>
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
   * The app's screen is up, perhaps under a system overlay (the phone's own call screen, Control Center, a
   * notification shade): only going to the background proper (SDL's DID_ENTER_BACKGROUND) makes it not visible.
   * IsForeground() turns false already when iOS resigns the app active (SDL's WILL_ENTER_BACKGROUND), which suits
   * pausing work but not deciding where a call rings. Desktop: always true.
   */
  static bool IsVisible();

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

  /**
   * A live session is on (a call ringing or in progress). In the background the app's work keeps
   * running meanwhile, so the session goes on: a hang-up, a network move, a ring answered from the
   * phone's own call screen. When it ends there, the work runs kLiveSessionEndGraceMs more (its last
   * signaling goes out), then pauses again (push only, P010). Any thread.
   */
  static void SetLiveSessionOn(bool on);
  static bool LiveSessionOn();
  static constexpr std::chrono::milliseconds kLiveSessionEndGraceMs{5000};

  /**
   * A push woke the app: in the background its work runs for `window` (a newer wake extends it),
   * long enough to fetch the inbox and ring (P010). Any thread.
   */
  static void OpenWakeWindow(std::chrono::milliseconds window);

  static void OnWillEnterBackground();
  /** Mobile: the app went to the background proper (not just an overlay): IsVisible() turns false. */
  static void OnDidEnterBackground();
  static void OnDidEnterForeground();
  static void OnLowMemory();

  static ListenerId AddBackgroundListener(std::function<void()> listener);
  static ListenerId AddForegroundListener(std::function<void()> listener);
  /**
   * The user may be looking at the app again: the phone came to the foreground, a desktop window was restored or
   * got input focus back. Not a promise (IsUserAttentive() can still be false, e.g. restored but unfocused): the
   * listener asks it. Called on the UI thread; removed with RemoveListener.
   */
  static ListenerId AddAttentiveListener(std::function<void()> listener);
  /**
   * A remote notification reached the app (iOS: the notification-center delegate and the remote-notification
   * callback; the system may have written its own icon number). Any thread: the listeners run on the UI thread.
   */
  static void OnRemoteNotification();
  static ListenerId AddRemoteNotificationListener(std::function<void()> listener);
  static void RemoveListener(ListenerId id);
};

} // namespace pbr
