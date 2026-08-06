#include "base/media/CallMediaHealth.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace pbr {
namespace {

bool& CliDiagnosticsOverrideFlag() {
  static bool flag = false;
  return flag;
}

} // namespace

void SetCallDiagnosticsCliOverride(bool enabled) {
  CliDiagnosticsOverrideFlag() = enabled;
}

bool CallDiagnosticsCliOverride() {
  return CliDiagnosticsOverrideFlag();
}

bool CallDiagnosticsEnabled(bool profile_pref) {
  return profile_pref || CallDiagnosticsCliOverride();
}

CallMediaHealthView EvaluateCallMediaHealth(const CallMediaHealthInput& in) {
  CallMediaHealthView out;
  out.engine = in.engine;
  out.hop = in.hop;
  out.path_kind = in.engine.sfu_mode ? "relay" : "direct";

  const double pressure = std::max(in.engine.path_pressure, in.hop.path_pressure);
  const int64_t now = in.now_ms;
  const int64_t rx_age =
      (in.engine.last_rx_audio_ms > 0 && now > in.engine.last_rx_audio_ms)
          ? (now - in.engine.last_rx_audio_ms)
          : (in.engine.last_rx_audio_ms > 0 ? 0 : 1'000'000);
  const int64_t tx_age =
      (in.engine.last_tx_audio_ms > 0 && now > in.engine.last_tx_audio_ms)
          ? (now - in.engine.last_tx_audio_ms)
          : (in.engine.last_tx_audio_ms > 0 ? 0 : 1'000'000);

  const bool rx_alive = in.engine.rx_audio_frames > 0 && rx_age < 1500;
  const bool tx_alive = in.engine.muted || (in.engine.tx_audio_frames > 0 && tx_age < 1500);

  if (in.reconnecting || (!in.engine.connected && in.engine.active)) {
    out.quality = CallPathQuality::Reconnecting;
    out.quality_bars = 1;
  } else if (in.engine.active && in.engine.connected && !rx_alive && tx_alive) {
    out.quality = CallPathQuality::NoAudio;
    out.asymmetry = CallAudioAsymmetry::SendingOnly;
    out.quality_bars = 0;
  } else if (in.engine.active && in.engine.connected && rx_alive && !tx_alive && !in.engine.muted) {
    out.quality = CallPathQuality::Poor;
    out.asymmetry = CallAudioAsymmetry::ReceivingOnly;
    out.quality_bars = 1;
  } else if (pressure >= 0.75 || in.hop.drops_total > 20) {
    out.quality = CallPathQuality::Poor;
    out.quality_bars = 1;
  } else if (pressure >= 0.4 || in.engine.playout_underruns > 10) {
    out.quality = CallPathQuality::Fair;
    out.quality_bars = 2;
  } else if (pressure >= 0.15) {
    out.quality = CallPathQuality::Good;
    out.quality_bars = 3;
  } else {
    out.quality = CallPathQuality::Excellent;
    out.quality_bars = 4;
  }
  return out;
}

const char* CallPathQualityLabelKey(CallPathQuality q) {
  switch (q) {
  case CallPathQuality::Excellent:
  case CallPathQuality::Good:
    return "";
  case CallPathQuality::Fair:
    return "call.quality.fair";
  case CallPathQuality::Poor:
    return "call.quality.poor";
  case CallPathQuality::Reconnecting:
    return "call.status.reconnecting";
  case CallPathQuality::NoAudio:
    return "call.quality.no_audio";
  }
  return "";
}

const char* CallPathQualityDetailsLabelKey(CallPathQuality q) {
  switch (q) {
  case CallPathQuality::Excellent:
    return "call.quality.excellent";
  case CallPathQuality::Good:
    return "call.quality.good";
  case CallPathQuality::Fair:
    return "call.quality.fair";
  case CallPathQuality::Poor:
    return "call.quality.poor";
  case CallPathQuality::Reconnecting:
    return "call.status.reconnecting";
  case CallPathQuality::NoAudio:
    return "call.quality.no_audio";
  }
  return "call.quality.good";
}

const char* CallAudioAsymmetryHintKey(CallAudioAsymmetry a) {
  switch (a) {
  case CallAudioAsymmetry::None:
    return "";
  case CallAudioAsymmetry::SendingOnly:
    return "call.quality.hint.sending_only";
  case CallAudioAsymmetry::ReceivingOnly:
    return "call.quality.hint.receiving_only";
  }
  return "";
}

std::string FormatCallDebugSubtitle(const CallMediaHealthView& v, int64_t now_ms) {
  const int64_t rx_age =
      (v.engine.last_rx_audio_ms > 0 && now_ms >= v.engine.last_rx_audio_ms)
          ? (now_ms - v.engine.last_rx_audio_ms)
          : -1;
  std::ostringstream out;
  out << (v.engine.sfu_mode ? "SFU" : "P2P") << " · ";
  out << (v.engine.opus_target_bps / 1000) << "k · p";
  const double p = std::max(v.engine.path_pressure, v.hop.path_pressure);
  out << std::fixed;
  out.precision(1);
  out << p;
  if (rx_age >= 0) {
    out << " · rx" << rx_age << "ms";
  }
  return out.str();
}

std::string FormatCallDetailsText(const CallMediaHealthView& v, int64_t now_ms, bool include_debug,
                                  const CallDetailsCopy& copy) {
  std::ostringstream out;
  out << copy.duration_heading << ": " << (copy.elapsed.empty() ? "—" : copy.elapsed) << "\n";
  out << copy.path_heading << ": " << copy.path_label << "\n";
  out << copy.quality_heading << ": " << copy.quality_label << "\n";
  out << copy.mic_heading << ": " << copy.mic_label << "\n";
  out << copy.incoming_heading << ": " << copy.incoming_label << "\n";
  if (!copy.asymmetry_hint.empty()) {
    out << copy.note_heading << ": " << copy.asymmetry_hint << "\n";
  }
  if (include_debug) {
    out << "\n— " << copy.diagnostics_heading << " —\n";
    out << "call_id: " << copy.call_id << "\n";
    out << FormatCallDebugSubtitle(v, now_ms) << "\n";
    out << "pressure_engine=" << v.engine.path_pressure << " hop=" << v.hop.path_pressure << "\n";
    out << "opus_bps=" << v.engine.opus_target_bps << " streams=" << v.engine.stream_count << "\n";
    out << "rx_frames=" << v.engine.rx_audio_frames << " tx_frames=" << v.engine.tx_audio_frames
        << "\n";
    out << "underrun=" << v.engine.playout_underruns << " plc=" << v.engine.plc_frames
        << " tx_drops=" << v.engine.outbound_drops << "\n";
    out << "hop_drops rate=" << v.hop.drops_rate << " queue=" << v.hop.drops_queue
        << " ceiling=" << v.hop.drops_ceiling << " total=" << v.hop.drops_total << "\n";
  }
  return out.str();
}

std::string FormatMediaHealthLogLine(const CallMediaHealthView& v, int64_t now_ms,
                                     const std::string& call_id) {
  const int64_t rx_age =
      (v.engine.last_rx_audio_ms > 0 && now_ms >= v.engine.last_rx_audio_ms)
          ? (now_ms - v.engine.last_rx_audio_ms)
          : -1;
  std::ostringstream out;
  out << "media_health call=" << call_id << " path=" << v.path_kind
      << " quality=" << static_cast<int>(v.quality) << " asymmetry=" << static_cast<int>(v.asymmetry)
      << " streams=" << v.engine.stream_count << " rx_frames=" << v.engine.rx_audio_frames
      << " tx_frames=" << v.engine.tx_audio_frames << " underrun=" << v.engine.playout_underruns
      << " plc=" << v.engine.plc_frames << " pressure=" << v.engine.path_pressure
      << " hop_pressure=" << v.hop.path_pressure << " opus_bps=" << v.engine.opus_target_bps
      << " tx_drops=" << v.engine.outbound_drops << " hop_drops_rate=" << v.hop.drops_rate
      << " hop_drops_queue=" << v.hop.drops_queue << " hop_drops_ceiling=" << v.hop.drops_ceiling
      << " rx_age_ms=" << rx_age;
  return out.str();
}

} // namespace pbr
