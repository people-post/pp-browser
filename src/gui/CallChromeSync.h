#pragma once

#include "lib/ui/ShellTypes.h"

#include <array>
#include <chrono>
#include <string>

namespace pbr {

/** Snapshot of call overlay layers used to decide Dirty vs SyncLayout. */
struct CallChromeLayer {
  bool ring_active = false;
  bool in_call_active = false;
  bool ring_pulse = false;
  bool in_call_muted = false;
  bool in_call_camera_on = false;
  bool in_call_speaker_on = false;
  bool in_call_stage_visible = false;
  bool in_call_remote_video = false;
  bool in_call_local_preview = false;
  bool ring_conflict = false;
  bool ring_show_pricing = false;
  bool ring_accept_charge_enabled = false;
  std::string ring_call_id;
  std::string in_call_id;
  std::string in_call_subtitle;
  std::string ring_caller_label;
  std::string ring_media_label;
  std::string ring_eyebrow;
  std::string ring_conflict_hint;
  std::string ring_accept_label;
  std::string ring_voice_answer_label;
  std::string ring_decline_label;
  std::string ring_pricing_label;
  std::string ring_accept_charge_label;
  std::string ring_accept_charge_hint;
  std::string ring_accept_short;
  std::string ring_voice_short;
  std::string in_call_title;
  int in_call_mic_level = 0;
  int in_call_peer_level = 0;
  std::string in_call_mic_hint;
  std::string in_call_peer_hint;
  std::string in_call_elapsed;
  std::string in_call_peer_label;
  std::string in_call_remote_placeholder;
  bool in_call_show_roster = false;
  bool in_call_show_invite = false;
  bool in_call_show_retry = false;
  bool in_call_show_speaker = false;
  bool in_call_show_camera = true;
  bool in_call_show_flip = false;
  int in_call_participant_count = 0;
  std::string in_call_status_hint;
  CallChromeMode in_call_mode = CallChromeMode::Expanded;
  std::string in_call_spotlight;
  bool in_call_spotlight_fill = true;
  int in_call_minimized_corner = 0;
  int in_call_quality_bars = 4;
  bool in_call_quality_ok = true;
  bool in_call_quality_warn = false;
  bool in_call_quality_error = false;
  std::string in_call_quality_label;
  std::string in_call_quality_hint;
  bool in_call_show_debug_subtitle = false;
  std::string in_call_debug_subtitle;
};

enum class CallChromeUpdate {
  None,      // idle poll / no visible change
  DirtyOnly, // subtitle/labels while layer already shown
  Remount,   // layer appear / disappear / switch call_id → RemountCallChrome mounts
  LevelsOnly, // only the audio meters changed while they are on screen → dirty kCallLevelVariables
};

/** Window-model variables behind the 1:1 audio meters; LevelsOnly dirties exactly these (B146). */
inline constexpr std::array<const char*, 4> kCallLevelVariables = {
    "call_in_progress_mic_level",
    "call_in_progress_peer_level",
    "call_in_progress_mic_hint",
    "call_in_progress_peer_hint",
};

/** Pure gate: remount on layer identity / control presence / mode / status kind; Dirty for icon toggles;
 * meter-only changes are LevelsOnly while the meters are on screen (Expanded, no roster) and None otherwise. */
CallChromeUpdate ClassifyCallChromeUpdate(const CallChromeLayer& synced, const CallChromeLayer& next);

/**
 * Holds the chrome last pushed to the shell and decides each poll's update (B146).
 * Meter-only changes are pushed at most every kLevelInterval; a held-back change keeps the pushed
 * snapshot where it was, so the next poll pushes the newest level even if nothing changes again.
 * `now` is a steady (monotonic) clock: wall-clock jumps must not freeze the meters.
 */
class CallChromeSyncGate {
public:
  static constexpr std::chrono::milliseconds kLevelInterval{200};

  CallChromeUpdate Decide(const CallChromeLayer& next, std::chrono::steady_clock::time_point now);

private:
  CallChromeLayer synced_;
  std::chrono::steady_clock::time_point last_levels_{};
  bool levels_pushed_ = false;
};

/** Group / roster calls open Immersive; 1:1 opens Expanded (V031). */
CallChromeMode DefaultCallChromeMode(bool show_roster);

} // namespace pbr
