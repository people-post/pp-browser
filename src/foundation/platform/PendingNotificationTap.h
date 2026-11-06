#pragma once

#include <chrono>
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
 * alert. Once synced, the ring or message with that id is the one the user tapped. A tap without a hint
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
   * replaces any earlier tap.
   */
  void Record(std::string thread_id, const bool call_wake, std::string wake_hint, const Clock::time_point now) {
    std::function<void()> changed;
    {
      std::lock_guard<std::mutex> lock(mu_);
      thread_id_ = std::move(thread_id);
      call_ = call_wake;
      hint_ = std::move(wake_hint);
      route_until_ = std::nullopt;
      if (call_wake) {
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
    if (!route_until_ || now > *route_until_) {
      ClearRoute();
      return false;
    }
    if (!hint_.empty() && hint_ != call_id) {
      return false;
    }
    ClearRoute();
    return true;
  }

  /** True once if a fresh message-alert tap is for the synced message `message_id`. An expired tap is dropped. */
  bool TakeMessageRoute(const std::string& message_id, const Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (call_ || hint_.empty()) {
      return false;
    }
    if (!route_until_ || now > *route_until_) {
      ClearRoute();
      return false;
    }
    if (hint_ != message_id) {
      return false;
    }
    ClearRoute();
    return true;
  }

private:
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
  std::function<void()> on_recorded_;
};

} // namespace pbr
