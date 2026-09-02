#pragma once

#include "domain/ui/ShellTypes.h"

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
  std::string ring_decline_label;
  std::string ring_pricing_label;
  std::string ring_accept_charge_label;
  std::string ring_accept_charge_hint;
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
  int in_call_participant_count = 0;
  std::string in_call_status_hint;
  CallChromeMode in_call_mode = CallChromeMode::Expanded;
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
};

/** Pure gate: remount on layer identity / control presence / mode / status kind; Dirty for icon toggles + meters. */
CallChromeUpdate ClassifyCallChromeUpdate(const CallChromeLayer& synced, const CallChromeLayer& next);

/** Group / roster calls open Immersive; 1:1 opens Expanded (V031). */
CallChromeMode DefaultCallChromeMode(bool show_roster);

} // namespace pbr
