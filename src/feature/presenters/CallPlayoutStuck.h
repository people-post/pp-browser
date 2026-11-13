#pragma once

#include <algorithm>
#include <cstdint>

namespace pbr {

/**
 * "Their voice arrives but nothing plays" (people-post/pp-browser #605: after the sender's audio pauses for a second or
 * two — the OS takes the audio, a hold, a speaker switch — the receiver's packets keep coming while its jitter buffer
 * stays empty and never plays again; only a new call helps). The existing health reads only whether packets arrive
 * (CallMediaHealth: rx_alive), so this fault looked healthy. This decides it from three engine counters, per tick.
 *
 * A window is the stretch from a baseline sample until the first sample >= kWindowMs later. It is "stuck" when, scaled
 * to kWindowMs by the real elapsed time, packets still came in (>= kMinRxFrames), nearly every playout slot ran dry
 * (>= kMinUnderruns), and the jitter depth was 0 at EVERY sample inside it (a poor but alive network has depth > 0
 * somewhere in two seconds). kStuckWindows windows in a row (about 6 s) make `stuck`; one window that is not stuck
 * clears it. Numbers: the 2026-11-13 three-way call logs — a healthy call raises underruns by 0-2 per 2 s, the fault by
 * about 100 per 2 s, while packets still rise by about 50 per 2 s.
 *
 * Nothing is counted (and the baseline is dropped) while the call is held here or there, reconnecting or not
 * connected: the first window after such a state starts from a fresh baseline, entirely after it. A counter that went
 * backwards (engine restart, rebuilt track) or a long gap between ticks drops the baseline too. No GUI, no SDL.
 */
class CallPlayoutStuck {
public:
  static constexpr int64_t kWindowMs = 2'000;
  static constexpr uint64_t kMinRxFrames = 30;
  static constexpr uint64_t kMinUnderruns = 30;
  static constexpr int kStuckWindows = 3;
  /** A longer gap between two ticks (the UI was suspended) says nothing about the playout: start over. */
  static constexpr int64_t kMaxGapMs = 5'000;

  struct Sample {
    /** Connected, and not on hold here or there, not reconnecting. False: nothing counts, the baseline goes. */
    bool counting = false;
    uint64_t rx_frames = 0;
    uint64_t underruns = 0;
    int64_t jitter_depth_ms = 0;
    int64_t now_ms = 0;
  };
  struct Result {
    bool stuck = false;
    /** This tick is the one where `stuck` turned true (once per stuck spell). */
    bool became_stuck = false;
  };

  Result Update(const Sample& s) {
    if (!s.counting) {
      Reset();
      return {};
    }
    const bool gap = have_base_ && s.now_ms - last_ms_ > kMaxGapMs;
    const bool backwards =
        have_base_ && (s.rx_frames < last_rx_ || s.underruns < last_underruns_ || s.now_ms < last_ms_);
    if (!have_base_ || gap || backwards) {
      Rebase(s);
      stuck_ = false;
      windows_ = 0;
      return {};
    }
    last_rx_ = s.rx_frames;
    last_underruns_ = s.underruns;
    last_ms_ = s.now_ms;
    max_depth_ms_ = std::max(max_depth_ms_, s.jitter_depth_ms);
    const int64_t elapsed = s.now_ms - base_ms_;
    if (elapsed < kWindowMs) {
      return {stuck_, false};
    }
    const uint64_t e = static_cast<uint64_t>(elapsed);
    const bool window_stuck = (s.rx_frames - base_rx_) * kWindowMs >= kMinRxFrames * e &&
                              (s.underruns - base_underruns_) * kWindowMs >= kMinUnderruns * e && max_depth_ms_ <= 0;
    windows_ = window_stuck ? windows_ + 1 : 0;
    const bool was = stuck_;
    stuck_ = windows_ >= kStuckWindows;
    Rebase(s);
    return {stuck_, stuck_ && !was};
  }

  bool stuck() const { return stuck_; }

  /** No call, or a state that does not count: everything starts over. */
  void Reset() { *this = CallPlayoutStuck{}; }

private:
  void Rebase(const Sample& s) {
    have_base_ = true;
    base_rx_ = last_rx_ = s.rx_frames;
    base_underruns_ = last_underruns_ = s.underruns;
    base_ms_ = last_ms_ = s.now_ms;
    max_depth_ms_ = s.jitter_depth_ms;
  }

  bool have_base_ = false;
  bool stuck_ = false;
  int windows_ = 0;
  uint64_t base_rx_ = 0;
  uint64_t base_underruns_ = 0;
  int64_t base_ms_ = 0;
  uint64_t last_rx_ = 0;
  uint64_t last_underruns_ = 0;
  int64_t last_ms_ = 0;
  int64_t max_depth_ms_ = 0;
};

} // namespace pbr
