#pragma once

#include <cstdint>
#include <limits>

namespace pbr {

/**
 * "The OS took the call's audio and has not given it back" (B217: after Siri the hold went on for ever). The hold is
 * this device's (CallLocalHold::OnHold); a normal one — Siri, a banner call — ends in 2-7 s, and while the app is in
 * front the audio session retries every 1.5 s. A hold that outlasts kHoldStuckAfterMs is one the user has to end by
 * hanging up and calling again: the status line then says so once (nothing else: no pill, no repeat, no hang-up).
 * This decides it from four facts per tick; the owner asked for "11 or 15 s, said once, and be sure it is this".
 *
 * Time counts only while the call is held AND the app is in front AND the call is not reconnecting (a network fault has
 * its own lines): a background tick pauses the count and keeps it. "Stuck" is only ever true on a tick in the
 * foreground, so a background tick never gives the rising edge; the first appearance also waits kForegroundGraceMs
 * after the last background tick and after the last reconnecting tick. Once shown, it stays (no hiding for a short trip
 * to the background; a reconnect gives way to its own line and the text comes back kForegroundGraceMs after it) until
 * the hold ends. A hold that ends and starts again within kHoldRejoinMs (the retry chain got the audio, lost it again)
 * goes on with the same count instead of starting over; a spell that was shown ends with the hold, whole.
 *
 * A tick gap above kMaxTickGapMs (the UI was suspended) or below 0 (the clock went back) adds nothing and clears
 * nothing. A spell that was shown ends once: with the hold (`Recovered`) or with the call (`Finish`, `CallEnded`); that
 * is when the metric is written. No GUI, no SDL.
 */
class CallHoldStuck {
public:
  /** About 12 s: the owner said 11 or 15; the retry chain has had about 8 tries by then. */
  static constexpr int64_t kHoldStuckAfterMs = 12'000;
  /** After the last background tick / reconnecting tick, the first appearance waits this long. */
  static constexpr int64_t kForegroundGraceMs = 3'000;
  static constexpr int64_t kMaxTickGapMs = 5'000;
  static constexpr int64_t kHoldRejoinMs = 2'000;

  enum class End { Recovered, CallEnded };

  struct Sample {
    /** This device's call is on hold (the OS has the audio). A user's own mute is not a hold. */
    bool held = false;
    /** The app is in front (AppLifecycle::IsForeground). */
    bool foreground = true;
    /** The call reconnects, cannot connect, or waits for recovery: not counted, and gives way. */
    bool reconnecting = false;
    int64_t now_ms = 0;
  };
  struct Result {
    /** The line says so now. */
    bool stuck = false;
    /** This tick is the one where it first showed (once per spell). */
    bool became_stuck = false;
    /** A spell that was shown ended on this tick (once per spell); `counted_ms` and `end` say how long and how. */
    bool ended = false;
    int64_t counted_ms = 0;
    End end = End::Recovered;
  };

  Result Update(const Sample& s) {
    Result r;
    if (!s.held) {
      if (shown_) {
        r.ended = true;
        r.counted_ms = counted_ms_;
        r.end = End::Recovered;
        Reset();
        return r;
      }
      if (counted_ms_ > 0) { // a hold that may come back at once: the count waits kHoldRejoinMs
        if (!released_) {
          released_ = true;
          released_ms_ = s.now_ms;
        } else if (s.now_ms - released_ms_ > kHoldRejoinMs || s.now_ms < released_ms_) {
          counted_ms_ = 0;
          released_ = false;
        }
      }
      last_counting_ = false;
      last_ms_ = s.now_ms;
      have_last_ = true;
      return r;
    }
    if (released_ && (s.now_ms - released_ms_ > kHoldRejoinMs || s.now_ms < released_ms_)) {
      counted_ms_ = 0;
    }
    released_ = false;
    const bool counting = s.foreground && !s.reconnecting;
    const int64_t dt = s.now_ms - last_ms_;
    if (have_last_ && last_counting_ && counting && dt > 0 && dt <= kMaxTickGapMs) {
      counted_ms_ += dt;
    }
    if (!s.foreground) {
      last_background_ms_ = s.now_ms;
    }
    if (s.reconnecting) {
      last_reconnecting_ms_ = s.now_ms;
    }
    last_counting_ = counting;
    last_ms_ = s.now_ms;
    have_last_ = true;

    const bool quiet_after_reconnect = s.now_ms - last_reconnecting_ms_ >= kForegroundGraceMs;
    if (shown_) {
      r.stuck = counting && quiet_after_reconnect;
    } else if (counting && counted_ms_ >= kHoldStuckAfterMs && quiet_after_reconnect &&
               s.now_ms - last_background_ms_ >= kForegroundGraceMs) {
      shown_ = true;
      r.stuck = true;
      r.became_stuck = true;
    }
    return r;
  }

  /** The call ended (or another began): a spell that was shown ends here, once. Everything starts over. */
  Result Finish() {
    Result r;
    if (shown_) {
      r.ended = true;
      r.counted_ms = counted_ms_;
      r.end = End::CallEnded;
    }
    Reset();
    return r;
  }

  /** No call: everything starts over, nothing is reported. */
  void Reset() { *this = CallHoldStuck{}; }

  int64_t counted_ms() const { return counted_ms_; }

private:
  static constexpr int64_t kLongAgo = std::numeric_limits<int64_t>::min() / 4;

  int64_t counted_ms_ = 0;
  bool shown_ = false;
  bool have_last_ = false;
  /** The last tick was held, in front and not reconnecting (its interval may be added to the next one's). */
  bool last_counting_ = false;
  int64_t last_ms_ = 0;
  int64_t last_background_ms_ = kLongAgo;
  int64_t last_reconnecting_ms_ = kLongAgo;
  /** The hold ended (first not-held tick at `released_ms_`) while a count was waiting. */
  bool released_ = false;
  int64_t released_ms_ = 0;
};

} // namespace pbr
