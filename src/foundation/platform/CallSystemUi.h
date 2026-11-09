#pragma once

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The phone's own call screen: the incoming-call notification on Android (P014, AndroidCallSystemUi). It rings
 * full screen, on the lock screen too, and answers / ends calls there. Null elsewhere, iPhone included
 * (Available() is false): the app's own ring and notifications do it all.
 *
 * Driven by CallSystemUiBridge on the UI thread; the callbacks arrive there too.
 */
class ICallSystemUi {
public:
  struct Callbacks {
    /**
     * The user answered on the system screen. On the push's call, its ring not known yet: `call_id` is the call
     * the push named, or empty when it named none.
     */
    std::function<void(const std::string& call_id)> answer;
    /** The user ended (declined, hung up) on the system screen; the push's call: as for `answer`. */
    std::function<void(const std::string& call_id)> end;
    std::function<void(const std::string& call_id, bool muted)> set_muted;
    /** The push's call left the system screen without a ring found (timeout, reset, ended by us). */
    std::function<void()> pending_gone;
  };
  enum class EndReason { Ended, AnsweredElsewhere, Failed };

  virtual ~ICallSystemUi() = default;

  /**
   * Rings can go here (Android: notifications allowed, the calls channel on). Cheap: the platform may keep the
   * answer (asking costs calls into the OS, and the bridge asks every frame of a call) until RecheckAvailable.
   */
  virtual bool Available() const { return false; }
  /**
   * Ask the OS again at the next Available(): the user may have changed the settings meanwhile. The bridge calls it
   * for each new ring; the platform also does when the app comes back to the foreground.
   */
  virtual void RecheckAvailable() {}
  virtual void SetCallbacks(Callbacks callbacks) { (void)callbacks; }
  /**
   * A call push reached the phone and its call is on the system screen ("Incoming call"), waiting
   * for the app to find the ring it announced.
   */
  virtual bool HasPendingPushCall() const { return false; }
  /** The call the push's pending call names; empty: none pending, or it named none. */
  virtual std::string PendingPushCallId() const { return {}; }
  /**
   * The app can find rings now (vault unlocked, messaging up). The push's call waits for its ring
   * unboundedly (up to an absolute cap) until this, then the usual short timeouts count from here.
   */
  virtual void NotifyMessagingReady() {}
  /**
   * Take the push's call off the system screen: no ring will match it. `reason`: AnsweredElsewhere when the app
   * took over (it joined a call on its own, its own ring rings), Ended when the call it names is over (P017).
   */
  virtual void EndPendingPushCall(EndReason reason) { (void)reason; }
  /**
   * `fn` runs (UI thread) whenever HasPendingPushCall() changes; the PIN screen words itself by it. The platform
   * keeps HasPendingPushCall() current by the same news, so reading it costs nothing.
   */
  virtual void SetPendingChangedHandler(std::function<void()> fn) { (void)fn; }
  /**
   * Ring `call_id` on the system screen. It fills in the push's pending call when there is one and the push named
   * this call or none; another ring gets a call of its own.
   */
  virtual void ReportIncoming(const std::string& call_id, const std::string& caller_name, bool video) {
    (void)call_id;
    (void)caller_name;
    (void)video;
  }
  /** `call_id` is over (or the app took it): take it off the system screen. */
  virtual void ReportEnded(const std::string& call_id, EndReason reason) {
    (void)call_id;
    (void)reason;
  }

  static ICallSystemUi& Instance();
  static void SetInstance(ICallSystemUi* ui);
};

} // namespace pbr
