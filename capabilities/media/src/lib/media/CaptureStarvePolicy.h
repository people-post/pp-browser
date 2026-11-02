#pragma once

#include <cstdint>

namespace pbr {

/**
 * When a mic that delivers no PCM gets reopened (B56, device test 2026-09-29).
 *
 * After ~500 ms without PCM the capture loop sends silence either way. An SDL device is reopened
 * then too (a wedged Android AudioRecord never recovers by itself). Voice processing (Apple VPIO)
 * starts slowly: opening takes 1.3–2.5 s on macOS and the first input can lag more than 500 ms.
 * Reopening it at 500 ms restarted it before it warmed up, and each reopen cost another ~2 s —
 * a loop that left the Mac mic silent for 15–60 s at the start of a call.
 */
struct CaptureStarvePolicy {
  /** No PCM this long: send silence (and reopen an SDL device). */
  static constexpr int64_t kSilenceAfterMs = 500;
  /** Voice processing gets this long after an open before a gap counts as starved. */
  static constexpr int64_t kVoiceWarmupMs = 3000;
  /** Past warm-up, a voice-processing gap this long is a stall (it delivers every ~85 ms). */
  static constexpr int64_t kVoiceStallMs = 1000;
  /** Starvation reopens only count as "in a row" until the unit has run this long after one. */
  static constexpr int64_t kVoiceHealthyMs = 5000;
  /** This many starvation reopens in a row: give up on voice processing for the call. */
  static constexpr int kVoiceMaxReopens = 3;

  /** How long without PCM before a reopen, given how long ago the device was (re)opened. */
  static int64_t ReopenAfterMs(bool voice_processing, int64_t since_open_ms) {
    if (!voice_processing) {
      return kSilenceAfterMs;
    }
    return since_open_ms < kVoiceWarmupMs ? kVoiceWarmupMs : kVoiceStallMs;
  }

  /** PCM arriving this long after an open ends a run of starvation reopens. */
  static bool HealthyAfterOpen(int64_t since_open_ms) { return since_open_ms >= kVoiceHealthyMs; }
};

} // namespace pbr
