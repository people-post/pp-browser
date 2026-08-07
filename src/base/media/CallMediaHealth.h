#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/** User-facing call path quality (Tier A). */
enum class CallPathQuality {
  Excellent = 0,
  Good = 1,
  Fair = 2,
  Poor = 3,
  Reconnecting = 4,
  NoAudio = 5,
};

enum class CallAudioAsymmetry {
  None = 0,
  /** Local TX alive, little/no remote audio. */
  SendingOnly = 1,
  /** Remote audio alive, little/no local send (and not muted). */
  ReceivingOnly = 2,
};

/** Per remote publisher stream (decoded RX). */
struct CallMediaStreamHealth {
  uint32_t stream_id = 0;
  uint64_t rx_frames = 0;
  int64_t last_rx_ms = 0;
  float peak_level = 0.f;
};

struct CallMediaEngineHealth {
  bool active = false;
  bool connected = false;
  bool sfu_mode = false;
  bool muted = false;
  double path_pressure = 0.0;
  int64_t opus_target_bps = 0;
  uint64_t outbound_drops = 0;
  uint64_t playout_underruns = 0;
  uint64_t plc_frames = 0;
  uint64_t rx_audio_frames = 0;
  uint64_t tx_audio_frames = 0;
  int64_t last_rx_audio_ms = 0;
  int64_t last_tx_audio_ms = 0;
  size_t stream_count = 0;
  float local_level = 0.f;
  float remote_level = 0.f;
  /** Per remote publisher (PreferLocal multi-peer dogfood). */
  std::vector<CallMediaStreamHealth> streams;
};

struct CallHopPeerHealth {
  std::string peer_id;
  int64_t bytes_up = 0;
  int64_t bytes_down = 0;
  uint64_t drops_rate = 0;
  uint64_t drops_queue = 0;
  size_t outbound_backlog = 0;
};

struct CallHopHealth {
  bool attached = false;
  double path_pressure = 0.0;
  uint64_t drops_rate = 0;
  uint64_t drops_queue = 0;
  uint64_t drops_ceiling = 0;
  uint64_t drops_total = 0;
  /** Remote (non-local) hop participants — for dogfood / media_health. */
  std::vector<CallHopPeerHealth> peers;
};

struct CallMediaHealthInput {
  CallMediaEngineHealth engine;
  CallHopHealth hop;
  /** Wall clock for age checks. */
  int64_t now_ms = 0;
  /** True while lifecycle/media is in an explicit reconnect / connect-pending state. */
  bool reconnecting = false;
};

struct CallMediaHealthView {
  CallPathQuality quality = CallPathQuality::Good;
  CallAudioAsymmetry asymmetry = CallAudioAsymmetry::None;
  /** 0..4 segment fill (0 empty … 4 full) — same language as statusbar reach bars. */
  int quality_bars = 4;
  /** `direct` or `relay`. */
  std::string path_kind = "direct";
  CallMediaEngineHealth engine;
  CallHopHealth hop;
};

/** Pure evaluation for chrome + logs (V032 instrumentation). */
CallMediaHealthView EvaluateCallMediaHealth(const CallMediaHealthInput& in);

/** i18n key for Tier A quality label (empty when Excellent/Good — chrome may omit). */
const char* CallPathQualityLabelKey(CallPathQuality q);
/** i18n key for Call details sheet (always non-empty). */
const char* CallPathQualityDetailsLabelKey(CallPathQuality q);
/** i18n key for asymmetry coaching hint (empty when None). */
const char* CallAudioAsymmetryHintKey(CallAudioAsymmetry a);

/** Compact debug subtitle: `SFU · 24k · p0.4 · rx120ms`. */
std::string FormatCallDebugSubtitle(const CallMediaHealthView& v, int64_t now_ms);

struct CallDetailsCopy {
  std::string elapsed;
  std::string path_label;     // already localized ("Direct" / "Via relay")
  std::string quality_label;  // already localized
  std::string mic_label;
  std::string incoming_label;
  std::string asymmetry_hint; // optional localized coaching
  std::string call_id;
  /** Localized field headings (defaults match English diagnostics). */
  std::string duration_heading = "Duration";
  std::string path_heading = "Path";
  std::string quality_heading = "Quality";
  std::string mic_heading = "Your mic";
  std::string incoming_heading = "Incoming audio";
  std::string note_heading = "Note";
  std::string diagnostics_heading = "Diagnostics";
};

/** Clipboard / alert body — normal fields always; debug section when `include_debug`. */
std::string FormatCallDetailsText(const CallMediaHealthView& v, int64_t now_ms, bool include_debug,
                                  const CallDetailsCopy& copy);

/** Single-line log: `media_health call=… …`. */
std::string FormatMediaHealthLogLine(const CallMediaHealthView& v, int64_t now_ms,
                                     const std::string& call_id);

/** CLI `--debug` ORs with profile pref (not persisted). */
void SetCallDiagnosticsCliOverride(bool enabled);
bool CallDiagnosticsCliOverride();
bool CallDiagnosticsEnabled(bool profile_pref);

} // namespace pbr
