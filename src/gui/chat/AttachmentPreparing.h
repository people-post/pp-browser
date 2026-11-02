#pragma once

#include <chrono>
#include <string>

namespace pbr {

/**
 * The gap on a phone between the person accepting a video (camera "Use Video", library pick) and the file path
 * arriving: the system copies it out first. While it lasts the composer shows "Sending…" and takes no second
 * attachment. Pure state: the chat controller starts it from the platform's `on_preparing` and ends it on every
 * way out (path delivered, nothing came, cancelled, failed); a deadline ends it anyway if no exit ever reports.
 */
class AttachmentPreparing {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds kTimeout{60};

  /** False (and nothing changes) when a preparation is already on: the second entry is refused. */
  bool Begin(Clock::time_point now = Clock::now()) {
    if (busy_) {
      return false;
    }
    busy_ = true;
    deadline_ = now + kTimeout;
    return true;
  }
  /** Every exit ends it. */
  void End() { busy_ = false; }
  /** Ends it when the deadline has passed; true only on the call that ended it. */
  bool Expire(Clock::time_point now) {
    if (busy_ && now >= deadline_) {
      busy_ = false;
      return true;
    }
    return false;
  }
  bool Busy() const { return busy_; }

private:
  bool busy_ = false;
  Clock::time_point deadline_{};
};

/**
 * The upload chip (and the input it locks) belong to the chat the attachment is sent in, like a draft: shown only
 * while that chat is the open one. `in_flight`: preparing, uploading, or a failed send waiting for Retry.
 */
inline bool AttachmentChipHere(const bool in_flight, const std::string& attachment_thread,
                               const std::string& active_thread) {
  return in_flight && attachment_thread == active_thread;
}

} // namespace pbr
