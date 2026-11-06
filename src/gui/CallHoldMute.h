#pragma once

#include <optional>

namespace pbr {

/**
 * Our microphone across a local hold (V060): while another call has this device's audio, the mic is muted so nothing
 * of that call can reach this one (sent like a manual mute, so the others see it off); when the hold ends it is
 * unmuted only if the hold muted it — a user who had muted stays muted. UI thread only.
 */
class CallHoldMute {
public:
  /** The hold began (`held`) or ended, the mic muted or not right now: the mute to apply (none: leave it). */
  std::optional<bool> Next(const bool held, const bool muted_now) {
    if (held) {
      muted_by_hold_ = !muted_now;
      return muted_by_hold_ ? std::optional<bool>(true) : std::nullopt;
    }
    if (!muted_by_hold_) {
      return std::nullopt;
    }
    muted_by_hold_ = false;
    return false;
  }

private:
  bool muted_by_hold_ = false;
};

} // namespace pbr
