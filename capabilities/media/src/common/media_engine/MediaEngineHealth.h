#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

// What a media engine reports about its running session (MediaEngine::HealthSnapshot): counters and
// levels for chrome, logs and the call summary. Neutral: calls turn it into call quality
// (common/media/CallMediaHealth), broadcast reads the same.

/** Per remote publisher stream (decoded RX). */
struct MediaStreamHealth {
  uint32_t stream_id = 0;
  uint64_t rx_frames = 0;
  int64_t last_rx_ms = 0;
  float peak_level = 0.f;
};

struct MediaEngineHealth {
  bool active = false;
  bool packet_mode = false;
  bool muted = false;
  /** False when mic open failed / absent — silence may still be sent; not a TX fault. */
  bool capture_available = true;
  double path_pressure = 0.0;
  int64_t opus_target_bps = 0;
  uint64_t outbound_drops = 0;
  uint64_t playout_underruns = 0;
  uint64_t plc_frames = 0;
  /** Frames recovered from the next packet's Opus in-band FEC (spec §2). */
  uint64_t fec_frames = 0;
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
  uint64_t rx_audio_frames = 0;
  uint64_t tx_audio_frames = 0;
  int64_t last_rx_audio_ms = 0;
  int64_t last_tx_audio_ms = 0;
  uint64_t rx_video_frames = 0;
  uint64_t tx_video_frames = 0;
  int64_t last_rx_video_ms = 0;
  int64_t video_target_bps = 0;
  size_t stream_count = 0;
  float local_level = 0.f;
  float remote_level = 0.f;
  /** Per remote publisher (PreferLocal multi-peer dogfood). */
  std::vector<MediaStreamHealth> streams;
};

} // namespace pbr
