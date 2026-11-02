#pragma once

#include "common/media_engine/MediaEngineHealth.h"

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

/**
 * Cumulative Reliable figures of the UDP association a call's media rides (Amp `ConnectionStats`;
 * a relayed call reads the association to its relay).
 */
struct CallLinkCounters {
  bool available = false;
  /**
   * Which link these are (unique per link instance in the mesh): a delta only means something
   * between two samples of the same link — the call switches between its 1:1 link and a hop's relay
   * link, and a path move can land on a long-lived link whose counters are already far along.
   */
  uint64_t link_id = 0;
  uint64_t reliable_sent = 0;
  uint64_t retransmits = 0;
  /** Smoothed round trip; -1 before the first sample. */
  int64_t srtt_ms = -1;
};

/**
 * What Call details shows of the call's link: the smoothed round trip, and the share of Reliable
 * (control) packets resent between two samples. Media is BestEffort — its loss shows in PLC / jitter,
 * not here — so this informs, it does not grade the call.
 */
struct CallLinkHealth {
  bool available = false;
  int64_t rtt_ms = -1;
  /** -1 when nothing Reliable was sent in between. */
  double resend_pct = -1.0;
};

CallLinkHealth CallLinkHealthBetween(const CallLinkCounters& before, const CallLinkCounters& now);

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
  /** The association to the relay (unavailable when attached to this device's own hop). */
  CallLinkCounters link;
};

struct CallMediaHealthInput {
  MediaEngineHealth engine;
  CallHopHealth hop;
  /** Wall clock for age checks. */
  int64_t now_ms = 0;
  /** True while lifecycle/media is in an explicit reconnect / connect-pending state. */
  bool reconnecting = false;
  /**
   * 1:1 reach mode from CallMediaBridge: direct | punched | circuit.
   * Used when hop is not attached (1:1 Amp also sets engine.sfu_mode for capture).
   */
  std::string reach_path_kind;
  /** The call's link, sampled by the caller (rates need two samples). */
  CallLinkHealth link;
};

struct CallMediaHealthView {
  CallPathQuality quality = CallPathQuality::Good;
  CallAudioAsymmetry asymmetry = CallAudioAsymmetry::None;
  /** 0..4 segment fill (0 empty … 4 full) — same language as statusbar reach bars. */
  int quality_bars = 4;
  /** direct | punched | circuit | media_relay (not status-bar Direct). */
  std::string path_kind = "direct";
  MediaEngineHealth engine;
  CallHopHealth hop;
  CallLinkHealth link;
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
  std::string network_label;  // optional localized link figures ("Round trip 42 ms · 1.2% resent")
  std::string call_id;
  /** Localized field headings (defaults match English diagnostics). */
  std::string duration_heading = "Duration";
  std::string path_heading = "Path";
  std::string quality_heading = "Quality";
  std::string mic_heading = "Your mic";
  std::string incoming_heading = "Incoming audio";
  std::string note_heading = "Note";
  std::string network_heading = "Network";
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
