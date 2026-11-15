#pragma once

#include <cstdint>
#include <functional>

namespace pbr {

/**
 * The phone's real lifecycle edges that AppLifecycle (capabilities/base) does not tell apart: SDL's
 * DID_ENTER_BACKGROUND (the app is really in the background, not only inactive — a lock screen, Control Center, a
 * banner or a Face ID prompt resign the app active without it), WILL_ENTER_FOREGROUND (really coming back) and
 * DID_ENTER_FOREGROUND (active, every time). Fed by
 * the SDL event watch (PlatformEarlyInit), on the UI thread; listeners run there, outside the lock.
 */
class PlatformLifecycleEvents {
public:
  using ListenerId = uint64_t;

  /** The app went to the background proper. AppLifecycle::IsVisible() is already false when this runs. */
  static ListenerId AddEnteredBackgroundListener(std::function<void()> listener);
  /** The app is on its way back from the background proper (the next "active" is a real return). */
  static ListenerId AddWillEnterForegroundListener(std::function<void()> listener);
  /**
   * The app is active (SDL DID_ENTER_FOREGROUND, "active" only). Unlike AppLifecycle's foreground listeners it is
   * called every time: those are skipped when AppLifecycle never saw a background (a cold start in the background).
   */
  static ListenerId AddDidEnterForegroundListener(std::function<void()> listener);
  static void RemoveListener(ListenerId id);

  /** Called by the SDL event watch, after AppLifecycle has taken the same event. Each also writes its log line. */
  static void OnDidEnterBackground();
  static void OnWillEnterForeground();
  static void OnDidEnterForeground();

  /** Monotonic milliseconds (steady clock): for gates and settle times; never the wall clock. */
  static int64_t MonoMs();
};

} // namespace pbr
