#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace pbr {

/**
 * The latest notification tap that has not been acted on yet. A tap can arrive before the vault is
 * unlocked (cold start, PIN screen), when there is no chat to open and no ring to route to; it waits
 * here. Only the newest tap is kept. Thread-safe: taps arrive off the UI thread.
 */
class PendingNotificationTap {
public:
  using Clock = std::chrono::steady_clock;
  /** How long a call-alert tap may wait for its ring to show up. */
  static constexpr std::chrono::seconds kCallRouteTtl{90};

  static PendingNotificationTap& Instance() {
    static PendingNotificationTap instance;
    return instance;
  }

  /** A message tap (`thread_id`) or a call-alert tap (`call_wake`, no thread yet); replaces any earlier tap. */
  void Record(std::string thread_id, const bool call_wake, const Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    thread_id_ = std::move(thread_id);
    call_route_until_ = call_wake ? std::optional<Clock::time_point>(now + kCallRouteTtl) : std::nullopt;
  }

  /** The chat a message tap asked for; taken once. */
  std::optional<std::string> TakeThread() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_id_.empty()) {
      return std::nullopt;
    }
    return std::exchange(thread_id_, std::string{});
  }

  /** True once if a call-alert tap is still fresh: the ring on screen is the one the user tapped. An expired tap is dropped. */
  bool TakeCallRoute(const Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool fresh = call_route_until_ && now <= *call_route_until_;
    call_route_until_.reset();
    return fresh;
  }

private:
  mutable std::mutex mu_;
  std::string thread_id_;
  std::optional<Clock::time_point> call_route_until_;
};

} // namespace pbr
