#pragma once

#include "common/media_engine/MediaEngineHealth.h"

#include <cstdint>
#include <optional>
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
  /** Frames the hop brought that no key held opened, since it was joined (a rotation's gap, V068; shut out, V067). */
  uint64_t unopened_frames = 0;
};

/** Path pressure (the engine's or the hop's, the larger) at which the call grades Poor, Fair, Good (else Excellent). */
inline constexpr double kCallPressurePoor = 0.75;
inline constexpr double kCallPressureFair = 0.4;
inline constexpr double kCallPressureGood = 0.15;
/** B231: the engine's buffer pressure grades the call Poor only once it has stayed at kCallPressurePoor this long. */
inline constexpr int64_t kCallBufferPressureSustainMs = 1000;
/** B231: the engine's buffer pressure is held (not graded anew) this long after a local audio route change. */
inline constexpr int64_t kCallRouteChangeSettleMs = 3000;
/** B231: after such a window ends, no other starts for this long (pressure is masked 3 s in any 13 s at most). */
inline constexpr int64_t kCallRouteChangeCooldownMs = 10000;
/** B231: evaluation that skips more than this lets the gate forget what it saw (a pause is not a steady state). */
inline constexpr int64_t kCallPressurePauseMs = 2000;

/**
 * B231: a local audio route change (the speaker toggle, the OS's route change) refills the jitter buffer — its depth
 * jumps and the engine's pressure reads 1 for up to ~2 s with no loss on the network — so the signal bars flashed weak
 * on every speaker tap. The engine's pressure that may grade the call: none within kCallRouteChangeSettleMs of a route
 * change, and a spike at kCallPressurePoor counts as Fair until it has lasted kCallBufferPressureSustainMs. The hop's
 * pressure and the loss signs (no audio in, none out) are not touched: they still grade at once.
 */
class CallBufferPressureGate {
public:
  /**
   * `route_changed_ms`: the last local route change (the speaker toggle, a device that came or went) on `now_ms`'s
   * clock (0: none). Call once per evaluation. While a window is running the gate returns what it graded before it
   * (a Poor call stays Poor), so a call that is really degrading is not shown healthy; only a change that is fresh
   * and after the cooldown opens a window, and one inside a window does not extend it.
   */
  double Step(double engine_pressure, int64_t now_ms, int64_t route_changed_ms);

private:
  /** When the engine's pressure reached kCallPressurePoor outside a window (0: not there). */
  int64_t poor_since_ms_ = 0;
  /** What the last evaluation outside a window returned. */
  double held_ = 0.0;
  int64_t last_step_ms_ = 0;
  /** The newest route change already looked at, so one change is judged once. */
  int64_t seen_route_change_ms_ = 0;
  /** End of the running or last window (0: none yet). */
  int64_t window_end_ms_ = 0;
};

struct CallMediaHealthInput {
  MediaEngineHealth engine;
  CallHopHealth hop;
  /** Wall clock for age checks. */
  int64_t now_ms = 0;
  /** The call's media was live and is not now (it reconnects or moves path): the call's state, not the engine's. */
  bool reconnecting = false;
  /**
   * 1:1 reach mode from CallDirectPath: direct | punched | circuit.
   * Used when hop is not attached (1:1 Amp also sets engine.packet_mode for capture).
   */
  std::string reach_path_kind;
  /** The call's link, sampled by the caller (rates need two samples). */
  CallLinkHealth link;
  /** The engine's pressure that grades the call (CallBufferPressureGate); none: engine.path_pressure as it is. */
  std::optional<double> engine_pressure_for_quality;
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
  std::string path_label;    // already localized ("Direct" / "Via relay")
  std::string quality_label; // already localized
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
std::string FormatMediaHealthLogLine(const CallMediaHealthView& v, int64_t now_ms, const std::string& call_id);

/** CLI `--debug` ORs with profile pref (not persisted). */
void SetCallDiagnosticsCliOverride(bool enabled);
bool CallDiagnosticsCliOverride();
bool CallDiagnosticsEnabled(bool profile_pref);

} // namespace pbr
