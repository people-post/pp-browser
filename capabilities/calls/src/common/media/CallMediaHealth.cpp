#include "common/media/CallMediaHealth.h"

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

double CallBufferPressureGate::Step(const double engine_pressure, const int64_t now_ms,
                                    const int64_t route_changed_ms) {
  if (last_step_ms_ != 0 && now_ms - last_step_ms_ > kCallPressurePauseMs) {
    // Evaluation paused (a ring over the call, another screen, the app in the background): the pressure seen before the
    // pause says nothing about the time since, so its persistence timer and the grade held for a window start over.
    poor_since_ms_ = 0;
    held_ = 0.0;
  }
  last_step_ms_ = now_ms;

  if (route_changed_ms > seen_route_change_ms_) {
    seen_route_change_ms_ = route_changed_ms;
    // A change whose settle time has already run out arms nothing. One that arrives inside a window does not restart
    // it, and none starts until kCallRouteChangeCooldownMs after the last window ended.
    const bool fresh = now_ms - route_changed_ms < kCallRouteChangeSettleMs;
    const bool idle = now_ms >= window_end_ms_;
    const bool rested = window_end_ms_ == 0 || route_changed_ms >= window_end_ms_ + kCallRouteChangeCooldownMs;
    if (fresh && idle && rested) {
      window_end_ms_ = route_changed_ms + kCallRouteChangeSettleMs;
    }
  }
  if (now_ms < window_end_ms_) {
    // Settling: keep showing what was graded before. A call that was already Poor stays Poor, its timer running.
    if (held_ < kCallPressurePoor) {
      poor_since_ms_ = 0; // a spike that began in the settle time is timed from its end
    }
    return held_;
  }
  if (engine_pressure < kCallPressurePoor) {
    poor_since_ms_ = 0;
    held_ = engine_pressure;
    return held_;
  }
  if (poor_since_ms_ == 0) {
    poor_since_ms_ = now_ms;
  }
  held_ = now_ms - poor_since_ms_ >= kCallBufferPressureSustainMs ? engine_pressure : kCallPressureFair;
  return held_;
}

CallMediaHealthView EvaluateCallMediaHealth(const CallMediaHealthInput& in) {
  CallMediaHealthView out;
  out.engine = in.engine;
  out.hop = in.hop;
  out.link = in.link;
  // media_relay only when hop SFU is attached — 1:1 Amp also uses engine.packet_mode for capture.
  if (in.hop.attached) {
    out.path_kind = "media_relay";
  } else if (!in.reach_path_kind.empty()) {
    out.path_kind = in.reach_path_kind;
  } else {
    out.path_kind = "direct";
  }

  const double pressure =
      std::max(in.engine_pressure_for_quality.value_or(in.engine.path_pressure), in.hop.path_pressure);
  const int64_t now = in.now_ms;
  const int64_t rx_age = (in.engine.last_rx_audio_ms > 0 && now > in.engine.last_rx_audio_ms)
                             ? (now - in.engine.last_rx_audio_ms)
                             : (in.engine.last_rx_audio_ms > 0 ? 0 : 1'000'000);
  const int64_t tx_age = (in.engine.last_tx_audio_ms > 0 && now > in.engine.last_tx_audio_ms)
                             ? (now - in.engine.last_tx_audio_ms)
                             : (in.engine.last_tx_audio_ms > 0 ? 0 : 1'000'000);

  const bool rx_alive = in.engine.rx_audio_frames > 0 && rx_age < 1500;
  // No mic (intentional headless / missing device) is not a TX fault — same as muted.
  const bool tx_expected = !in.engine.muted && in.engine.capture_available;
  const bool tx_alive = !tx_expected || (in.engine.tx_audio_frames > 0 && tx_age < 1500);

  if (in.reconnecting) {
    out.quality = CallPathQuality::Reconnecting;
    out.quality_bars = 1;
  } else if (in.engine.active && !rx_alive && tx_alive) {
    out.quality = CallPathQuality::NoAudio;
    out.asymmetry = CallAudioAsymmetry::SendingOnly;
    out.quality_bars = 0;
  } else if (in.engine.active && rx_alive && !tx_alive && tx_expected) {
    out.quality = CallPathQuality::Poor;
    out.asymmetry = CallAudioAsymmetry::ReceivingOnly;
    out.quality_bars = 1;
  } else if (pressure >= kCallPressurePoor) {
    out.quality = CallPathQuality::Poor;
    out.quality_bars = 1;
  } else if (pressure >= kCallPressureFair) {
    out.quality = CallPathQuality::Fair;
    out.quality_bars = 2;
  } else if (pressure >= kCallPressureGood) {
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
      return "call.quality.hint.connecting_voice"; // voice wording; callers pick the video one
    case CallAudioAsymmetry::ReceivingOnly:
      return "call.quality.hint.receiving_only";
  }
  return "";
}

std::string FormatCallDebugSubtitle(const CallMediaHealthView& v, int64_t now_ms) {
  const int64_t rx_age = (v.engine.last_rx_audio_ms > 0 && now_ms >= v.engine.last_rx_audio_ms)
                             ? (now_ms - v.engine.last_rx_audio_ms)
                             : -1;
  std::ostringstream out;
  out << (v.path_kind.empty() ? (v.engine.packet_mode ? "SFU" : "P2P") : v.path_kind) << " · ";
  out << (v.engine.opus_target_bps / 1000) << "k · p";
  const double p = std::max(v.engine.path_pressure, v.hop.path_pressure);
  {
    std::ostringstream pressure;
    pressure << std::fixed;
    pressure.precision(1);
    pressure << p;
    out << pressure.str();
  }
  if (rx_age >= 0) {
    // Cap so a stalled peer does not look like a bogus multi-million counter.
    const int64_t shown = rx_age > 9999 ? 9999 : rx_age;
    out << " · rx" << shown << "ms";
  }
  return out.str();
}

CallLinkHealth CallLinkHealthBetween(const CallLinkCounters& before, const CallLinkCounters& now) {
  CallLinkHealth health;
  if (!now.available) {
    return health;
  }
  health.available = true;
  health.rtt_ms = now.srtt_ms;
  // Another link (path move, hop attach / detach): no rate across the switch.
  if (before.available && before.link_id == now.link_id && now.reliable_sent >= before.reliable_sent &&
      now.retransmits >= before.retransmits) {
    const uint64_t sent = now.reliable_sent - before.reliable_sent;
    if (sent > 0) {
      // Retransmits in the window can belong to packets first sent before it: cap at every one resent.
      const double pct = 100.0 * static_cast<double>(now.retransmits - before.retransmits) / static_cast<double>(sent);
      health.resend_pct = std::min(pct, 100.0);
    }
  }
  return health;
}

std::string FormatCallDetailsText(const CallMediaHealthView& v, int64_t now_ms, bool include_debug,
                                  const CallDetailsCopy& copy) {
  std::ostringstream out;
  out << copy.duration_heading << ": " << (copy.elapsed.empty() ? "—" : copy.elapsed) << "\n";
  out << copy.path_heading << ": " << copy.path_label << "\n";
  out << copy.quality_heading << ": " << copy.quality_label << "\n";
  out << copy.mic_heading << ": " << copy.mic_label << "\n";
  out << copy.incoming_heading << ": " << copy.incoming_label << "\n";
  if (!copy.network_label.empty()) {
    out << copy.network_heading << ": " << copy.network_label << "\n";
  }
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
        << " rx_video=" << v.engine.rx_video_frames << " tx_video=" << v.engine.tx_video_frames << "\n";
    out << "underrun=" << v.engine.playout_underruns << " plc=" << v.engine.plc_frames << " fec=" << v.engine.fec_frames
        << " tx_drops=" << v.engine.outbound_drops << "\n";
    out << "hop_drops rate=" << v.hop.drops_rate << " queue=" << v.hop.drops_queue << " ceiling=" << v.hop.drops_ceiling
        << " total=" << v.hop.drops_total << "\n";
  }
  return out.str();
}

std::string FormatMediaHealthLogLine(const CallMediaHealthView& v, int64_t now_ms, const std::string& call_id) {
  const int64_t rx_age = (v.engine.last_rx_audio_ms > 0 && now_ms >= v.engine.last_rx_audio_ms)
                             ? (now_ms - v.engine.last_rx_audio_ms)
                             : -1;
  std::ostringstream out;
  out << "media_health call=" << call_id << " path=" << v.path_kind << " quality=" << static_cast<int>(v.quality)
      << " asymmetry=" << static_cast<int>(v.asymmetry) << " streams=" << v.engine.stream_count
      << " rx_frames=" << v.engine.rx_audio_frames << " tx_frames=" << v.engine.tx_audio_frames
      << " rx_video=" << v.engine.rx_video_frames << " tx_video=" << v.engine.tx_video_frames
      << " preview=" << v.engine.preview_frames << " vstage=" << v.engine.video_stage
      << " vstage_ms=" << v.engine.video_stage_ms << " vfetch_ms=" << v.engine.video_fetch_ms
      << " enc_ms=" << static_cast<int64_t>(v.engine.encode_ms_avg) << "/"
      << static_cast<int64_t>(v.engine.encode_ms_max) << " video_bps=" << v.engine.video_target_bps
      << " underrun=" << v.engine.playout_underruns << " plc=" << v.engine.plc_frames << " fec=" << v.engine.fec_frames
      << " audio_io=" << v.engine.audio_io << " io_underrun=" << v.engine.io_underruns
      << " jb_target_ms=" << v.engine.jitter_target_ms << " jb_depth_ms=" << v.engine.jitter_depth_ms
      << " jb_silence_drops=" << v.engine.jitter_silence_drops << " jb_speech_drops=" << v.engine.jitter_speech_drops
      << " jb_late_drops=" << v.engine.jitter_late_drops << " pressure=" << v.engine.path_pressure
      << " hop_pressure=" << v.hop.path_pressure << " opus_bps=" << v.engine.opus_target_bps
      << " tx_drops=" << v.engine.outbound_drops << " hop_drops_rate=" << v.hop.drops_rate
      << " hop_drops_queue=" << v.hop.drops_queue << " hop_drops_ceiling=" << v.hop.drops_ceiling
      << " rx_age_ms=" << rx_age << " mic_lvl=" << v.engine.local_level << " peer_lvl=" << v.engine.remote_level;
  if (v.link.available) {
    out << " link_rtt_ms=" << v.link.rtt_ms << " link_resend_pct=" << v.link.resend_pct;
  }
  if (!v.engine.streams.empty()) {
    out << " rx_streams=";
    for (size_t i = 0; i < v.engine.streams.size(); ++i) {
      const auto& s = v.engine.streams[i];
      if (i > 0) {
        out << ',';
      }
      const int64_t age = (s.last_rx_ms > 0 && now_ms >= s.last_rx_ms) ? (now_ms - s.last_rx_ms) : -1;
      out << s.stream_id << ":n=" << s.rx_frames << "/age=" << age << "/lvl=" << s.peak_level;
    }
  }
  if (!v.hop.peers.empty()) {
    out << " hop_peers=";
    for (size_t i = 0; i < v.hop.peers.size(); ++i) {
      const auto& p = v.hop.peers[i];
      if (i > 0) {
        out << ',';
      }
      // Short peer id suffix for log scanability.
      const std::string& id = p.peer_id;
      const std::string short_id = id.size() > 8 ? id.substr(id.size() - 8) : id;
      out << short_id << ":up=" << p.bytes_up << "/dn=" << p.bytes_down << "/dq=" << p.drops_queue
          << "/bl=" << p.outbound_backlog;
    }
  }
  return out.str();
}

} // namespace pbr
