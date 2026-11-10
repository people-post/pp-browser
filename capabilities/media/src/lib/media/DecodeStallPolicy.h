#pragma once

#include <cstdint>
#include <string_view>

namespace pbr {

/**
 * The error a decoder returns once it has taken many frames and produced none for a long time
 * (B125). The engine requests a keyframe on it and, when it repeats, rebuilds the decoder.
 */
inline constexpr std::string_view kDecoderStalledError = "decoder stalled";

/**
 * Whether a hardware decoder that accepts input but gives no output is stalled, not starting up
 * (B125: Android MediaCodec takes several frames before its first output). Pure; times in ms.
 * Stalled = at least kStallFrames frames queued AND at least kStallMs since the last output (or the
 * configure), not counting frames queued while a requested keyframe is still on its way.
 */
class DecodeStallPolicy {
public:
  static constexpr int kStallFrames = 60;
  static constexpr int64_t kStallMs = 5000;
  /** After a keyframe request, frames are not counted for this long (the IDR is on its way). */
  static constexpr int64_t kKeyframeWaitMs = 3000;

  /** The decoder was (re)configured or rebuilt: counting starts over. */
  void Restart(int64_t now_ms) {
    frames_since_output_ = 0;
    last_output_ms_ = now_ms;
    keyframe_wait_until_ms_ = 0;
  }
  void NoteOutput(int64_t now_ms) {
    frames_since_output_ = 0;
    last_output_ms_ = now_ms;
  }
  /** A stall was reported: the next one needs another kStallFrames frames and kStallMs. */
  void NoteStallReported(int64_t now_ms) { NoteOutput(now_ms); }
  /** A keyframe was asked for (any decode error makes the engine ask). */
  void NoteKeyframeRequested(int64_t now_ms) { keyframe_wait_until_ms_ = now_ms + kKeyframeWaitMs; }
  void NoteFrameQueued(int64_t now_ms) {
    if (!WaitingForKeyframe(now_ms)) {
      ++frames_since_output_;
    }
  }
  bool Stalled(int64_t now_ms) const {
    return !WaitingForKeyframe(now_ms) && frames_since_output_ >= kStallFrames && now_ms - last_output_ms_ >= kStallMs;
  }
  int FramesSinceOutput() const { return frames_since_output_; }
  int64_t MsSinceOutput(int64_t now_ms) const { return now_ms - last_output_ms_; }

private:
  bool WaitingForKeyframe(int64_t now_ms) const { return now_ms < keyframe_wait_until_ms_; }

  int frames_since_output_ = 0;
  int64_t last_output_ms_ = 0;
  int64_t keyframe_wait_until_ms_ = 0;
};

/**
 * "Drop every frame until the next IDR" after a lost frame or a rebuild (B125). The keyframe request
 * or the IDR itself can be lost too, so while waiting the decoder asks again every kAskAgainMs
 * (a decode error makes the engine ask). Pure; times in steady-clock ms.
 */
class IdrWait {
public:
  static constexpr int64_t kAskAgainMs = 2000;

  /** Start waiting (a keyframe was just asked for, or will be by the caller's error). */
  void Start(int64_t now_ms) {
    waiting_ = true;
    asked_ms_ = now_ms;
  }
  /** An IDR arrived. */
  void Stop() { waiting_ = false; }
  bool Waiting() const { return waiting_; }
  /** A frame was dropped while waiting: true when it is time to ask for the keyframe again. */
  bool AskAgain(int64_t now_ms) {
    if (!waiting_ || now_ms - asked_ms_ < kAskAgainMs) {
      return false;
    }
    asked_ms_ = now_ms;
    return true;
  }

private:
  bool waiting_ = false;
  int64_t asked_ms_ = 0;
};

/**
 * When the engine rebuilds a stalled decoder (B125): after kStallsBeforeReset stalls in a row (no
 * frame between), at most kMaxResets times per call and at least kMinResetGapMs apart — a rebuild
 * runs under the media lock, so it must not repeat. Pure; times in steady-clock ms.
 */
class DecoderResetBudget {
public:
  static constexpr int kStallsBeforeReset = 2;
  static constexpr int kMaxResets = 2;
  static constexpr int64_t kMinResetGapMs = 10'000;

  /** A decode reported stalled. True when the decoder should be rebuilt now. */
  bool NoteStall(int64_t now_ms) {
    ++consecutive_stalls_;
    if (consecutive_stalls_ < kStallsBeforeReset || resets_ >= kMaxResets) {
      return false;
    }
    if (resets_ > 0 && now_ms - last_reset_ms_ < kMinResetGapMs) {
      return false;
    }
    ++resets_;
    last_reset_ms_ = now_ms;
    consecutive_stalls_ = 0;
    return true;
  }
  /** A frame came out: the stalls before it no longer run in a row. */
  void NoteFrame() { consecutive_stalls_ = 0; }
  int Resets() const { return resets_; }
  bool Exhausted() const { return resets_ >= kMaxResets; }

private:
  int consecutive_stalls_ = 0;
  int resets_ = 0;
  int64_t last_reset_ms_ = 0;
};

} // namespace pbr
