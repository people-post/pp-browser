#pragma once

#include "common/Diag.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace pbr {

/**
 * The latest notification tap that has not been acted on yet. A tap can arrive before the vault is
 * unlocked (cold start, PIN screen), when there is no chat to open and no ring to route to; it waits
 * here. Only the newest tap is kept. Thread-safe: taps arrive off the UI thread.
 *
 * A push tap carries the push's `wake_hint`: the call id of a call alert, the message id of a message
 * alert. Once synced, the ring, message or call summary with that id is the one the user tapped. A tap without a hint
 * (an older push) falls back to the first ring.
 */
class PendingNotificationTap {
public:
  using Clock = std::chrono::steady_clock;
  /** How long a call-alert tap may wait for its ring to show up. */
  static constexpr std::chrono::seconds kCallRouteTtl{90};
  /** How long a message-alert tap may wait for its message to sync (the PIN entry comes first). */
  static constexpr std::chrono::seconds kMessageRouteTtl{180};

  static PendingNotificationTap& Instance() {
    static PendingNotificationTap instance;
    return instance;
  }

  /**
   * A banner tap (`thread_id`), a call-alert tap (`call_wake`), or a message-alert tap (`wake_hint` only);
   * replaces any earlier tap. `notification_age`: how long before the tap the tapped notification was delivered
   * (kept for a call-alert tap only; nullopt = unknown).
   */
  void Record(std::string thread_id, const bool call_wake, std::string wake_hint, const Clock::time_point now,
              const std::optional<std::chrono::milliseconds> notification_age = std::nullopt) {
    std::function<void()> changed;
    {
      std::lock_guard<std::mutex> lock(mu_);
      thread_id_ = std::move(thread_id);
      call_ = call_wake;
      hint_ = std::move(wake_hint);
      route_until_ = std::nullopt;
      miss_logged_ = false;
      Report("recorded", thread_id_.empty() ? (call_wake ? Kind::Call : Kind::Msg) : Kind::Thread);
      if (call_wake) {
        ++call_tap_seq_;
        call_tap_age_ = notification_age;
        call_tapped_at_ = now;
        route_until_ = now + kCallRouteTtl;
      } else if (!hint_.empty()) {
        route_until_ = now + kMessageRouteTtl;
      }
      changed = on_recorded_;
    }
    if (changed) {
      changed();
    }
  }

  /** Counts the call-alert taps ever recorded; never reset, so two taps are told apart even once one is taken. */
  uint64_t CallTapSeq() const {
    std::lock_guard<std::mutex> lock(mu_);
    return call_tap_seq_;
  }

  /** How old the notification of the latest call-alert tap was when tapped; nullopt when unknown. */
  std::optional<std::chrono::milliseconds> CallTapNotificationAge() const {
    std::lock_guard<std::mutex> lock(mu_);
    return call_tap_age_;
  }

  /** How long ago the latest call-alert tap was recorded; nullopt when there has been none. */
  std::optional<std::chrono::milliseconds> CallTapElapsed(const Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!call_tapped_at_) {
      return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - *call_tapped_at_);
  }

  /** `fn` runs on the thread that records a tap (not necessarily the UI thread), after the tap is stored. */
  void SetRecordedHandler(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mu_);
    on_recorded_ = std::move(fn);
  }

  /** The chat a banner tap asked for; taken once. */
  std::optional<std::string> TakeThread() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_id_.empty()) {
      return std::nullopt;
    }
    return std::exchange(thread_id_, std::string{});
  }

  /** True while a call-alert tap is fresh and its ring has not been routed yet; does not consume. */
  bool PeekCallRoute(const Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    return call_ && route_until_ && now <= *route_until_;
  }

  /**
   * True once if a fresh call-alert tap is for the ring `call_id`: with a hint the ids must match (another
   * ring leaves the tap waiting), without one any ring does. An expired tap is dropped.
   */
  bool TakeCallRoute(const std::string& call_id, const Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!call_) {
      return false;
    }
    if (!Fresh(now)) {
      return false;
    }
    if (!hint_.empty() && hint_ != call_id) {
      ReportMismatch();
      return false;
    }
    Report("routed", Kind::Call);
    ClearRoute();
    return true;
  }

  /** The hint of a fresh message or call tap, or empty; an expired tap is dropped. Does not consume. */
  std::string FreshHint(const Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    return hint_.empty() || !Fresh(now) ? std::string{} : hint_;
  }

  /**
   * True once if a fresh tap's hint is `id`: a synced message's id, or the call id of a call summary that
   * synced. A tap without a hint never matches. An expired tap is dropped.
   */
  bool TakeHintedRoute(const std::string& id, const Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (hint_.empty() || !Fresh(now)) {
      return false;
    }
    if (hint_ != id) {
      ReportMismatch();
      return false;
    }
    Report("routed", CurrentKind());
    ClearRoute();
    return true;
  }

private:
  enum class Kind { Msg, Call, Thread };

  Kind CurrentKind() const { return call_ ? Kind::Call : Kind::Msg; }

  void Report(const ShareWord what, const Kind kind) const {
    DiagLine line("notification.tap");
    line.Add("what", what);
    switch (kind) {
      case Kind::Call:
        line.Add("kind", "call");
        break;
      case Kind::Thread:
        line.Add("kind", "thread");
        break;
      case Kind::Msg:
        line.Add("kind", "msg");
        break;
    }
    line.Add("has_hint", !hint_.empty()).Emit();
  }

  /** Whether a route is still waiting and in time; an expired one is reported and dropped. Caller holds `mu_`. */
  bool Fresh(const Clock::time_point now) {
    if (!route_until_) {
      return false;
    }
    if (now > *route_until_) {
      Report("expired", CurrentKind());
      ClearRoute();
      return false;
    }
    return true;
  }

  /** At most one line per tap: every synced message that is not the tapped one would otherwise log. */
  void ReportMismatch() {
    if (!miss_logged_) {
      miss_logged_ = true;
      Report("mismatch", CurrentKind());
    }
  }

  void ClearRoute() {
    call_ = false;
    hint_.clear();
    route_until_.reset();
  }

  mutable std::mutex mu_;
  std::string thread_id_;
  bool call_ = false;
  std::string hint_;
  std::optional<Clock::time_point> route_until_;
  bool miss_logged_ = false;
  uint64_t call_tap_seq_ = 0;
  std::optional<std::chrono::milliseconds> call_tap_age_;
  std::optional<Clock::time_point> call_tapped_at_;
  std::function<void()> on_recorded_;
};

} // namespace pbr
