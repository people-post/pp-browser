#pragma once

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The phone's own call screen: CallKit on iPhone builds that may use it (P013). It rings full
 * screen, on the lock screen too, and answers / ends calls there. Null elsewhere (Available() is
 * false): the app's own ring and notifications do it all.
 *
 * Driven by CallSystemUiBridge on the UI thread; the callbacks arrive there too.
 */
class ICallSystemUi {
public:
  struct Callbacks {
    /** The user answered on the system screen. `call_id` empty: the push's call, its ring not known yet. */
    std::function<void(const std::string& call_id)> answer;
    /** The user ended (declined, hung up) on the system screen; empty: the push's call. */
    std::function<void(const std::string& call_id)> end;
    std::function<void(const std::string& call_id, bool muted)> set_muted;
  };
  enum class EndReason { Ended, AnsweredElsewhere, Failed };

  virtual ~ICallSystemUi() = default;

  virtual bool Available() const { return false; }
  virtual void SetCallbacks(Callbacks callbacks) { (void)callbacks; }
  /**
   * A call push reached the phone and its call is on the system screen ("Incoming call"), waiting
   * for the app to find the ring it announced.
   */
  virtual bool HasPendingPushCall() const { return false; }
  /** Ring `call_id` on the system screen (fills in the push's pending call when there is one). */
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
