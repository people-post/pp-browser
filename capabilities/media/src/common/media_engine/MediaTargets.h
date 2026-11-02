#pragma once

#include <cstdint>

namespace pbr {

/**
 * What a media engine sends at (MediaEngine::ApplyTargets). The feature's adaptation policy decides
 * it (calls: CallMediaAdaptation); the engine only applies it.
 */
struct MediaTargets {
  /** The Opus target before any policy says otherwise (a healthy path). */
  static constexpr int64_t kDefaultAudioBps = 24'000;

  /** False: the camera goes off (the path cannot carry video). */
  bool camera_allowed = true;
  /** Opus target; 0 keeps kDefaultAudioBps. */
  int64_t audio_bps = kDefaultAudioBps;
  /** Video encoder target; 0 leaves the encoder's own (or the video level's). */
  int64_t video_bps = 0;
};

} // namespace pbr
