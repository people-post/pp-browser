#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/** App media roles for V024 (map to N021 channel_type at SFU edge). */
enum class CallMediaRole {
  Audio,
  VideoLo,
  VideoHi,
};

struct CallAdaptationInput {
  /** Per-user uplink allowance (A↑). 0 = unknown/unbounded. */
  int64_t per_user_up_bps = 0;
  /** Observed/estimated send bitrate currently in use (optional). */
  int64_t current_send_bps = 0;
  bool camera_user_wants = false;
  bool muted = false;
  /** Network/path pressure 0..1 (0 = healthy). */
  double path_pressure = 0.0;
  /** When true, dual-layer hi is eligible (a5); thin a4 keeps false. */
  bool allow_video_hi = false;
};

struct CallAdaptationDecision {
  bool publish_audio = true;
  bool publish_video_lo = false;
  bool publish_video_hi = false;
  /** Camera hardware may stay on only when publish_video_lo || publish_video_hi. */
  bool camera_allowed = false;
  int64_t target_audio_bps = 32'000;
  int64_t target_video_lo_bps = 0;
  int64_t target_video_hi_bps = 0;
  std::string reason;
};

/**
 * Shared V024 adaptation brain for 1:1 P2P and SFU backends.
 * Priority: audio ≫ video_lo ≫ video_hi. Producer-first; single video layer OK for a4.
 */
class CallMediaAdaptation {
public:
  static constexpr int64_t kMinAudioBps = 16'000;
  static constexpr int64_t kDefaultAudioBps = 32'000;
  /** Comfortable encode target when path is healthy (engine default before quote). */
  static constexpr int64_t kComfortAudioBps = 24'000;
  static constexpr int64_t kDefaultVideoLoBps = 400'000;
  static constexpr int64_t kDefaultVideoHiBps = 1'200'000;
  static constexpr int64_t kMinVideoLoBps = 120'000;

  static CallAdaptationDecision Evaluate(const CallAdaptationInput& in);

  /** Relay quote want_up_bps from session video policy (blind bytes — V022). */
  static int64_t QuoteWantUpBps(bool video_allowed);

  /** Map path_pressure 0..1 → Opus target within [kMinAudioBps, comfort]. */
  static int64_t AudioBpsForPressure(double path_pressure, int64_t comfort_bps = kComfortAudioBps);

  /** N021 mapping for SFU path. */
  static const char* ChannelTypeName(CallMediaRole role);
};

/** Soft-migrate / hop topology helpers (V021 + V025). */
enum class CallMediaPathAction {
  StayP2p,
  SoftMigrateSfu,
  UseSfu,
  IgnoreSfuHint,
};

class CallMediaTopology {
public:
  /** Prefer media_relay only when joined count ≥ 3 (V025 — never auto-SFU plain 1:1). */
  static bool ShouldUseMediaRelay(size_t joined_count);

  /** Soft-migrate when growing from P2P (N=2) onto SFU (N≥3). */
  static bool ShouldSoftMigrateToSfu(size_t previous_joined, size_t new_joined);

  /** Pure path decision for roster N (and optional invite sfu_hint). */
  static CallMediaPathAction DecidePath(size_t joined_count, bool has_sfu_hint);
};

} // namespace pbr
