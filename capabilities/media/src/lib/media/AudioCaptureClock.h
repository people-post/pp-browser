#pragma once

#include <algorithm>
#include <cstdint>

namespace pbr {

/**
 * The capture time of each sent audio packet (the media frame's capture_ts, V066): ms since the sender session
 * started, of the packet's first sample.
 *
 * Counted in the packets' own length while capture streams, so the jitter of reading the microphone never moves it.
 * A capture gap — the microphone stalled or reopened, the OS held the session, nothing to send — shows as a jump: a
 * packet whose first sample the steady clock puts more than kGapMs after the count is stamped at the clock. The count
 * never steps back (a receiver schedules by it); when it trails the clock on every packet of a window — a microphone
 * clock slower than the steady one — it is slewed 1 ms toward it, so the drift never adds up to a false gap.
 */
class AudioCaptureClock {
public:
  /** Later than the count by more than this is a capture gap, not read jitter. */
  static constexpr int64_t kGapMs = 100;
  /** Packets (2 s of 40 ms) over which the count must trail the clock before it is slewed 1 ms. */
  static constexpr int kSlewPackets = 50;

  /**
   * The stamp of a packet of `packet_ms` whose first sample the steady clock puts at `first_sample_ms` (ms since the
   * session started: the reading when the packet was taken, less the audio queued up to its end). Wraps at 2^32 ms.
   */
  uint32_t Stamp(const int64_t first_sample_ms, const int packet_ms) {
    const int64_t clock = std::max<int64_t>(0, first_sample_ms);
    if (!started_ || clock - next_ms_ > kGapMs) {
      next_ms_ = std::max(next_ms_, clock);
      StartWindow();
    } else {
      Slew(clock - next_ms_);
    }
    started_ = true;
    const int64_t stamp = next_ms_;
    next_ms_ += packet_ms;
    return static_cast<uint32_t>(stamp);
  }

private:
  void StartWindow() {
    window_ = 0;
    min_lag_ = INT64_MAX;
  }

  /** `lag`: how far the clock is after the count for this packet (< 0: the count is ahead). */
  void Slew(const int64_t lag) {
    min_lag_ = std::min(min_lag_, lag);
    if (++window_ < kSlewPackets) {
      return;
    }
    if (min_lag_ > 0) {
      ++next_ms_;
    }
    StartWindow();
  }

  bool started_ = false;
  /** The capture time of the next packet's first sample, counted. */
  int64_t next_ms_ = 0;
  int window_ = 0;
  int64_t min_lag_ = INT64_MAX;
};

} // namespace pbr
