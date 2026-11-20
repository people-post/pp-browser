#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

// What a media engine reports about its running session (MediaEngine::HealthSnapshot): counters and
// levels for chrome, logs and the call summary. Neutral: calls turn it into call quality
// (common/media/CallMediaHealth), broadcast reads the same.

/** One remote video stream's arrivals so far: how evenly its frames came, not how they looked (VideoArrivalStats). */
struct VideoArrivalSummary {
  uint64_t frames = 0;
  /** Frames the sender's numbering skipped when a later one came (one that still comes after that stays counted). */
  uint64_t lost = 0;
  /** Time between consecutive arrivals: a sender at 20 fps spaces them about 50 ms apart. */
  int64_t gap_p50_ms = 0;
  int64_t gap_p95_ms = 0;
  int64_t gap_max_ms = 0;
  /** Gaps of at least VideoArrivalStats::kStallMs: the picture froze. */
  uint64_t stalls = 0;
  /** Frames that came within VideoArrivalStats::kBunchedMs of the one before: held back, then let go together. */
  uint64_t bunched = 0;
  /**
   * Keyframes, and the gaps that ended with one (sum and longest): how long a keyframe took to arrive after the
   * frame before it — near the frame period when it comes whole at once, longer when it is paced or queued.
   */
  uint64_t keyframes = 0;
  int64_t keyframe_gap_total_ms = 0;
  int64_t keyframe_gap_max_ms = 0;
};

struct VideoStreamArrival {
  uint32_t stream_id = 0;
  VideoArrivalSummary arrival;
};

/** Per remote publisher stream (decoded RX). */
struct MediaStreamHealth {
  uint32_t stream_id = 0;
  uint64_t rx_frames = 0;
  int64_t last_rx_ms = 0;
  float peak_level = 0.f;
};

/** A camera frame fetch that has run longer than this is a thread stuck in the capture stack (SDL), not a quiet camera. */
inline constexpr int64_t kVideoThreadStuckMs = 2000;

/** Opening or closing the camera device that takes longer than this is stuck too (the stage names which). */
inline constexpr int64_t kVideoCameraStageStuckMs = 5000;

/**
 * How long the video thread has been stuck, 0 when it is not: inside one frame fetch for longer than
 * kVideoThreadStuckMs (`video_fetch_ms`, 0 = not inside one), or in the open_camera / close_camera stage for longer
 * than kVideoCameraStageStuckMs. Pure: the caller keeps the once-per-spell edge.
 */
inline int64_t VideoThreadStuckMs(const int64_t video_fetch_ms, const std::string& video_stage,
                                  const int64_t video_stage_ms) {
  if (video_fetch_ms > kVideoThreadStuckMs) {
    return video_fetch_ms;
  }
  if ((video_stage == "open_camera" || video_stage == "close_camera") && video_stage_ms > kVideoCameraStageStuckMs) {
    return video_stage_ms;
  }
  return 0;
}

inline bool VideoThreadStuck(const int64_t video_fetch_ms, const std::string& video_stage,
                             const int64_t video_stage_ms) {
  return VideoThreadStuckMs(video_fetch_ms, video_stage, video_stage_ms) > 0;
}

/**
 * B233: a microphone is wanted for the call but no capture device is open. What the engine can tell from its own state,
 * with nothing else changed by asking: the session captures, the engine runs, the user has not muted, and no device
 * is open. Not counted (the silence has a known reason of its own): the OS permission is denied or still unanswered, the
 * OS has the audio session (a phone call, Siri), or a route change is queued or running.
 */
struct MicWantedInputs {
  bool engine_running = false;
  bool capture_wanted = false;
  bool muted = false;
  bool capture_device_open = false;
  bool permission_granted = true;
  bool os_has_session = false;
  bool route_change_pending = false;
};

inline bool MicWantedButNotOpen(const MicWantedInputs& in) {
  return in.engine_running && in.capture_wanted && !in.muted && !in.capture_device_open && in.permission_granted &&
         !in.os_has_session && !in.route_change_pending;
}

/**
 * How long MicWantedButNotOpen has held, on a monotonic clock the caller supplies; any thread. Read-only with respect
 * to the audio path: it only watches. Any tick where the state does not hold starts the count over.
 */
class MicMissingClock {
public:
  struct Reading {
    /** Milliseconds the state has held (at least 1 while it does), 0 when it does not. */
    int64_t missing_ms = 0;
    /** This reading is the first of a spell. */
    bool began = false;
    /** This reading ends a spell that had held this long (0: no spell ended). */
    int64_t ended_after_ms = 0;
    /** A device has been open at some reading (this one included). */
    bool ever_open = false;
  };

  Reading Step(const bool missing, const bool device_open, const int64_t now_ms) {
    Reading r;
    if (device_open) {
      ever_open_.store(true, std::memory_order_relaxed);
    }
    r.ever_open = ever_open_.load(std::memory_order_relaxed);
    if (!missing) {
      if (const int64_t was = since_ms_.exchange(0, std::memory_order_acq_rel); was != 0) {
        r.ended_after_ms = std::max<int64_t>(1, now_ms - was);
      }
      return r;
    }
    int64_t expected = 0;
    r.began = since_ms_.compare_exchange_strong(expected, now_ms, std::memory_order_acq_rel);
    const int64_t since = r.began ? now_ms : expected;
    r.missing_ms = std::max<int64_t>(1, now_ms - since);
    return r;
  }

private:
  /** When the spell began (0: none; a monotonic reading, never 0 in practice). */
  std::atomic<int64_t> since_ms_{0};
  std::atomic<bool> ever_open_{false};
};

struct MediaEngineHealth {
  bool active = false;
  bool packet_mode = false;
  bool muted = false;
  /** False when mic open failed / absent — silence may still be sent; not a TX fault. */
  bool capture_available = true;
  /**
   * The mic gives nothing even after a reopen and an audio rebuild (Android: the OS took the recording stream):
   * the other side hears silence until captured audio comes back, which clears it.
   */
  bool capture_lost = false;
  /**
   * B233: a microphone is wanted and no capture device has been open for this long (ms; MicWantedButNotOpen,
   * MicMissingClock); 0 when that is not so — also while muted, while the OS permission is not granted, while the OS
   * holds the session, and during a route change. Read-only: nothing is retried or reopened because of it.
   */
  int64_t mic_missing_ms = 0;
  /** A capture device was open at some sample of this session (the call summary's `mic`). */
  bool mic_ever_open = false;
  double path_pressure = 0.0;
  int64_t opus_target_bps = 0;
  uint64_t outbound_drops = 0;
  uint64_t playout_underruns = 0;
  uint64_t plc_frames = 0;
  /** Frames recovered from the next packet's Opus in-band FEC (spec §2). */
  uint64_t fec_frames = 0;
  /** Audio played that did not arrive in time (ms): loss concealment and FEC, summed over streams (V065). */
  uint64_t concealed_ms = 0;
  /** Live audio device path: "vpio" (OS voice processing / AEC), "sdl", or "none". */
  std::string audio_io = "none";
  /** The engine was started and not stopped (with or without audio devices). A sample taken after it
   *  stopped has all-zero counters; the call summary keeps the last running sample instead (B125). */
  bool engine_running = false;
  /** VPIO render callbacks padded with zeros, per call, after playout started (0 on SDL). */
  uint64_t io_underruns = 0;
  /** Adaptive jitter target (max over remote streams), and catch-up drops summed over streams. */
  int64_t jitter_target_ms = 0;
  /** Actual jitter buffer depth (max over remote streams) — how full it is against the target. */
  int64_t jitter_depth_ms = 0;
  uint64_t jitter_silence_drops = 0;
  uint64_t jitter_speech_drops = 0;
  /** Packets that came after their playout slot had passed (dropped; their slot was concealed). */
  uint64_t jitter_late_drops = 0;
  uint64_t rx_audio_frames = 0;
  uint64_t tx_audio_frames = 0;
  int64_t last_rx_audio_ms = 0;
  int64_t last_tx_audio_ms = 0;
  uint64_t rx_video_frames = 0;
  uint64_t tx_video_frames = 0;
  /** Frames shown in the self-view (the camera's rate, ~30 fps; video is sent at half of it). */
  uint64_t preview_frames = 0;
  /**
   * The video thread's current stage (a fixed word: "idle", "wait_camera", "wait_frame", ...) and how long it has been
   * in it (0 while the thread is not running). A stage that repeats (no frame, sleep 5 ms, again) does not move its
   * clock, so a thread stuck inside the camera's frame fetch is told apart by `video_fetch_ms`.
   */
  std::string video_stage = "idle";
  int64_t video_stage_ms = 0;
  /** How long the current camera frame fetch has been running (0: not inside one). */
  int64_t video_fetch_ms = 0;
  /** The encoder's time per frame this session: average and slowest (ms). */
  double encode_ms_avg = 0;
  double encode_ms_max = 0;
  int64_t last_rx_video_ms = 0;
  int64_t video_target_bps = 0;
  size_t stream_count = 0;
  float local_level = 0.f;
  float remote_level = 0.f;
  /** Per remote publisher (PreferLocal multi-peer dogfood). */
  std::vector<MediaStreamHealth> streams;
  /** Per remote video stream: how its frames arrived (before decoding). */
  std::vector<VideoStreamArrival> video_arrivals;
};

} // namespace pbr
