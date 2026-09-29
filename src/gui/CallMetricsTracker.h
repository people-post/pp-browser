#pragma once

#include "common/Metrics.h"
#include "common/media/CallMediaHealth.h"
#include "domain/media/DeviceVitals.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * Per-call operational metrics (DEV/PLAN-client-metrics.md). The UI feeds what it already knows —
 * ring, accept, outbound start, media ticks, device vitals, frame ticks — and gets back the Metrics
 * lines to emit:
 *   call.setup   once, when media connects or when the call ends without connecting
 *   call.summary once, when a connected call ends
 *   ui.stall     a UI frame gap over kStallMs during a call
 *   device.thermal a thermal state change during a call
 * Pure (no clocks, no devices, no logging) so it is unit-tested. UI thread only.
 */
class CallMetricsTracker {
public:
  /** The call counts as over once neither active nor ringing for this long (accept gaps). */
  static constexpr int64_t kEndGraceMs = 3000;
  /** A UI frame gap this long during a call is a stall (overload; watchdog kills start ~10 s). */
  static constexpr int64_t kStallMs = 1000;
  /** Longer gaps are the app being suspended, not a stall. */
  static constexpr int64_t kStallMaxMs = 60'000;

  struct MediaTick {
    bool connected = false;
    std::string path;  // direct / punched / relayed / hop …
    CallPathQuality quality = CallPathQuality::Good;
    CallMediaEngineHealth health;
  };

  /** Placing a call. Returns the previous call's final line if one was still tracked. */
  std::vector<std::string> NoteOutbound(const std::string& call_id, bool video, int64_t now_ms,
                                        const DeviceVitals& vitals) {
    std::vector<std::string> out = Begin(call_id, "caller", video, now_ms, vitals);
    call_.outbound_ms = now_ms;
    return out;
  }

  /** The ring dialog for an incoming call. Returns the previous call's final line, if any. */
  std::vector<std::string> NoteRing(const std::string& call_id, bool video, int64_t now_ms,
                                    const DeviceVitals& vitals) {
    if (call_.id == call_id) {
      return {};
    }
    std::vector<std::string> out = Begin(call_id, "callee", video, now_ms, vitals);
    call_.ring_ms = now_ms;
    return out;
  }

  void NoteAccept(const std::string& call_id, bool voice_only, int64_t now_ms) {
    if (call_.id != call_id || call_.accept_ms != 0) {
      return;
    }
    call_.accept_ms = now_ms;
    if (voice_only) {
      call_.video = false;
    }
  }

  void NoteDecline(const std::string& call_id) {
    if (call_.id == call_id) {
      call_.declined = true;
    }
  }

  /** Each media-health tick of the active call; returns lines to emit (call.setup on connect). */
  std::vector<std::string> NoteMedia(const std::string& call_id, const MediaTick& tick, int64_t now_ms,
                                     const DeviceVitals& vitals) {
    std::vector<std::string> out;
    if (call_.id != call_id || call_id.empty()) {
      return out;
    }
    NoteVitals(vitals, out);
    const CallMediaEngineHealth& h = tick.health;
    if (!call_.have_base) {
      call_.base = h;
      call_.have_base = true;
    }
    if (tick.connected && call_.connected_ms == 0) {
      call_.connected_ms = now_ms;
      call_.path = tick.path;
      call_.base = h;  // counters for the call start here
      out.push_back(SetupLine("connected", now_ms));
    }
    if (call_.connected_ms == 0) {
      return out;
    }
    // Connected from here on: path changes, reconnects, quality time, first frames, jitter.
    if (!tick.path.empty() && tick.path != call_.path) {
      ++call_.path_changes;
      call_.path = tick.path;
    }
    if (!tick.connected && call_.was_connected) {
      ++call_.reconnects;
    }
    call_.was_connected = tick.connected;
    const int64_t dt = call_.last_media_tick_ms ? now_ms - call_.last_media_tick_ms : 0;
    call_.last_media_tick_ms = now_ms;
    const int q = std::clamp(static_cast<int>(tick.quality), 0, 5);
    call_.quality_ms[q] += dt;
    if (tick.quality == CallPathQuality::NoAudio) {
      call_.no_audio_ms += dt;
    }
    if (call_.first_audio_ms < 0 && Delta(h.rx_audio_frames, call_.base.rx_audio_frames) > 0) {
      call_.first_audio_ms = now_ms - call_.connected_ms;
    }
    if (call_.first_video_ms < 0 && Delta(h.rx_video_frames, call_.base.rx_video_frames) > 0) {
      call_.first_video_ms = now_ms - call_.connected_ms;
    }
    call_.jb_target_max_ms = std::max(call_.jb_target_max_ms, h.jitter_target_ms);
    call_.jb_target_sum_ms += h.jitter_target_ms;
    ++call_.jb_samples;
    call_.last = h;
    return out;
  }

  /**
   * Every UI frame. call_live: a call is still under way (lifecycle not Idle, or the UI shows one).
   * Returns ui.stall for a long frame gap during a call, and the final line once no call has been
   * live for kEndGraceMs.
   */
  std::vector<std::string> Tick(bool call_live, int64_t now_ms, const DeviceVitals& vitals) {
    std::vector<std::string> out;
    const int64_t gap = last_frame_ms_ ? now_ms - last_frame_ms_ : 0;
    last_frame_ms_ = now_ms;
    if (call_.id.empty()) {
      return out;
    }
    if (gap >= kStallMs && gap < kStallMaxMs) {
      ++call_.stalls;
      call_.stall_max_ms = std::max(call_.stall_max_ms, gap);
      out.push_back(MetricsLine("ui.stall").Add("call", MetricsCallId(call_.id)).Add("gap_ms", gap).str());
    }
    if (call_live) {
      call_.last_seen_ms = now_ms;
      return out;
    }
    if (now_ms - call_.last_seen_ms < kEndGraceMs) {
      return out;
    }
    out.push_back(EndLine(call_.last_seen_ms, vitals));
    call_ = {};
    return out;
  }

  bool Tracking() const { return !call_.id.empty(); }

private:
  struct Call {
    std::string id;
    std::string role;
    bool video = false;
    bool declined = false;
    int64_t outbound_ms = 0;
    int64_t ring_ms = 0;
    int64_t accept_ms = 0;
    int64_t connected_ms = 0;
    int64_t last_seen_ms = 0;
    int64_t last_media_tick_ms = 0;
    std::string path;
    int path_changes = 0;
    int reconnects = 0;
    bool was_connected = true;
    int64_t quality_ms[6] = {0, 0, 0, 0, 0, 0};
    int64_t no_audio_ms = 0;
    int64_t first_audio_ms = -1;
    int64_t first_video_ms = -1;
    int64_t jb_target_max_ms = 0;
    int64_t jb_target_sum_ms = 0;
    int64_t jb_samples = 0;
    int stalls = 0;
    int64_t stall_max_ms = 0;
    bool have_base = false;
    CallMediaEngineHealth base;
    CallMediaEngineHealth last;
    DeviceVitals vitals_start;
    int thermal_max = -1;
    int thermal_last = -1;
  };

  static uint64_t Delta(uint64_t now, uint64_t base) { return now >= base ? now - base : now; }

  std::vector<std::string> Begin(const std::string& call_id, const char* role, bool video, int64_t now_ms,
                                 const DeviceVitals& vitals) {
    std::vector<std::string> out;
    if (!call_.id.empty() && call_.id != call_id) {
      out.push_back(EndLine(call_.last_seen_ms, vitals));
    }
    call_ = {};
    call_.id = call_id;
    call_.role = role;
    call_.video = video;
    call_.last_seen_ms = now_ms;
    call_.vitals_start = vitals;
    call_.thermal_max = vitals.thermal;
    call_.thermal_last = vitals.thermal;
    return out;
  }

  void NoteVitals(const DeviceVitals& vitals, std::vector<std::string>& out) {
    if (vitals.thermal < 0 || vitals.thermal == call_.thermal_last) {
      return;
    }
    out.push_back(MetricsLine("device.thermal")
                      .Add("call", MetricsCallId(call_.id))
                      .Add("state", ThermalStateName(vitals.thermal))
                      .Add("from", ThermalStateName(call_.thermal_last))
                      .str());
    call_.thermal_last = vitals.thermal;
    call_.thermal_max = std::max(call_.thermal_max, vitals.thermal);
  }

  std::string SetupLine(const char* result, int64_t now_ms) const {
    MetricsLine line("call.setup");
    line.Add("call", MetricsCallId(call_.id)).Add("role", call_.role).Add("video", call_.video).Add("result", result);
    if (call_.ring_ms && call_.accept_ms) {
      line.Add("answer_ms", call_.accept_ms - call_.ring_ms);
    }
    if (call_.connected_ms) {
      // Callee: from its own Accept. Caller: from placing the call (includes the callee's ringing).
      const int64_t from = call_.accept_ms ? call_.accept_ms : call_.outbound_ms;
      if (from) {
        line.Add("connect_ms", call_.connected_ms - from);
      }
      line.Add("path", call_.path);
    } else {
      const int64_t from = call_.accept_ms ? call_.accept_ms : (call_.outbound_ms ? call_.outbound_ms : call_.ring_ms);
      line.Add("waited_ms", now_ms - from);
    }
    return line.str();
  }

  std::string EndLine(int64_t end_ms, const DeviceVitals& vitals) const {
    if (call_.connected_ms == 0) {
      const char* result = call_.declined                               ? "declined"
                           : call_.role == "callee" && !call_.accept_ms ? "missed"
                           : call_.role == "callee"                     ? "failed"
                                                                        : "not_connected";
      return SetupLine(result, end_ms);
    }
    const CallMediaEngineHealth& b = call_.base;
    const CallMediaEngineHealth& l = call_.last;
    const int64_t duration_ms = std::max<int64_t>(end_ms - call_.connected_ms, 1);
    int64_t quality_total = 0;
    for (const int64_t ms : call_.quality_ms) {
      quality_total += ms;
    }
    auto pct = [&](int64_t ms) { return quality_total > 0 ? static_cast<int64_t>(ms * 100 / quality_total) : 0; };
    const double secs = static_cast<double>(duration_ms) / 1000.0;
    MetricsLine line("call.summary");
    line.Add("call", MetricsCallId(call_.id))
        .Add("role", call_.role)
        .Add("video", call_.video)
        .Add("duration_s", secs)
        .Add("path", call_.path)
        .Add("path_changes", call_.path_changes)
        .Add("reconnects", call_.reconnects)
        .Add("first_audio_ms", call_.first_audio_ms)
        .Add("first_video_ms", call_.first_video_ms)
        .Add("audio_io", l.audio_io)
        .Add("rx_audio", Delta(l.rx_audio_frames, b.rx_audio_frames))
        .Add("tx_audio", Delta(l.tx_audio_frames, b.tx_audio_frames))
        .Add("underrun", Delta(l.playout_underruns, b.playout_underruns))
        .Add("plc", Delta(l.plc_frames, b.plc_frames))
        .Add("fec", Delta(l.fec_frames, b.fec_frames))
        .Add("jb_target_avg_ms", call_.jb_samples ? call_.jb_target_sum_ms / call_.jb_samples : int64_t{0})
        .Add("jb_target_max_ms", call_.jb_target_max_ms)
        .Add("jb_drops", Delta(l.jitter_silence_drops + l.jitter_speech_drops,
                               b.jitter_silence_drops + b.jitter_speech_drops))
        .Add("rx_fps", static_cast<double>(Delta(l.rx_video_frames, b.rx_video_frames)) / secs)
        .Add("tx_fps", static_cast<double>(Delta(l.tx_video_frames, b.tx_video_frames)) / secs)
        .Add("q_excellent_pct", pct(call_.quality_ms[0]))
        .Add("q_good_pct", pct(call_.quality_ms[1]))
        .Add("q_fair_pct", pct(call_.quality_ms[2]))
        .Add("q_poor_pct", pct(call_.quality_ms[3]))
        .Add("q_reconnecting_pct", pct(call_.quality_ms[4]))
        .Add("q_noaudio_pct", pct(call_.quality_ms[5]))
        .Add("no_audio_s", static_cast<double>(call_.no_audio_ms) / 1000.0)
        .Add("ui_stalls", call_.stalls)
        .Add("ui_stall_max_ms", call_.stall_max_ms);
    if (call_.vitals_start.cpu_s >= 0 && vitals.cpu_s >= 0) {
      // Whole process, from ring / dial to the end — the call's power-use proxy.
      line.Add("cpu_s", vitals.cpu_s - call_.vitals_start.cpu_s);
    }
    line.Add("thermal_max", ThermalStateName(std::max(call_.thermal_max, vitals.thermal)));
    if (call_.vitals_start.battery_pct >= 0) {
      line.Add("battery_start", call_.vitals_start.battery_pct)
          .Add("battery_end", vitals.battery_pct)
          .Add("charging", call_.vitals_start.charging || vitals.charging);
    }
    return line.str();
  }

  Call call_;
  int64_t last_frame_ms_ = 0;
};

} // namespace pbr
