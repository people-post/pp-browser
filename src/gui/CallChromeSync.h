#pragma once

#include "lib/ui/ShellTypes.h"

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace pbr {

/** Snapshot of call overlay layers used to decide Dirty vs SyncLayout. */
struct CallChromeLayer {
  bool ring_active = false;
  bool in_call_active = false;
  bool ring_pulse = false;
  bool in_call_muted = false;
  bool in_call_camera_on = false;
  bool in_call_camera_front = false;
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
  /** V062 5: written into the ring's markup (a change remounts). */
  std::string ring_guest_hint;
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
  /** G022: "Make a group…" and the group banner's words (written into the markup: a change remounts). */
  bool in_call_show_make_group = false;
  std::string in_call_group_invite;
  /** V062: the guest rows (written into the markup: a change remounts). */
  std::string in_call_guests;
  bool in_call_show_retry = false;
  bool in_call_calling = false;
  bool in_call_show_speaker = false;
  bool in_call_show_camera = true;
  bool in_call_show_flip = false;
  int in_call_participant_count = 0;
  std::string in_call_status_hint;
  CallChromeMode in_call_mode = CallChromeMode::Expanded;
  std::string in_call_spotlight;
  bool in_call_spotlight_fill = true;
  int in_call_minimized_side = 0;
  int in_call_minimized_y_permille = 0;
  bool in_call_minimized_tucked = false;
  int in_call_quality_bars = 4;
  bool in_call_quality_ok = true;
  bool in_call_quality_warn = false;
  bool in_call_quality_error = false;
  std::string in_call_quality_label;
  std::string in_call_quality_hint;
  bool in_call_show_debug_subtitle = false;
  std::string in_call_debug_subtitle;

  /** The roster rows' bound fields in one key: a mute or camera change in a row dirties the call group (the clock
   * tick no longer does, so it cannot carry them any more). */
  std::string in_call_roster;
  /** The phone page's speaking rings (the rows' `speaking`, kept out of in_call_roster): part-dirtied, never the
   * whole call group nor a remount (B146). */
  std::string in_call_speaking;

  // Phone call page (ShellCallChromeRml). Written into the markup: a change remounts.
  bool phone = false;
  std::string ring_phone_title;
  std::string ring_phone_subtitle;
  std::string ring_avatar; // group / photo / letter / tone in one key
  /** Written into the phone ring markup (dimmed, click-less answers): a change remounts. */
  bool ring_awaiting_invite = false;
  /** Bound (the silhouette variant's data-if): a change dirties. */
  bool ring_caller_unknown = false;
  /** Bound: the ring page's three lines (words + flags) and our own avatar on its left. Changes dirty. */
  std::string ring_steps;
  std::string in_call_phone_title;
  bool in_call_video = false;
  std::string in_call_avatars; // the peer's and our own, as for ring_avatar
  bool in_call_phone_failed = false;
  std::string in_call_failed_text;
  bool in_call_story_active = false;
  /** Bound (a stage advance, the step words and the list's scroll only dirty): the stage, the step words, the offset. */
  int in_call_story_stage = 0;
  std::string in_call_story_steps;
  std::string in_call_story_scroll;
  /** Written into the markup (a change remounts): the list's shape, i.e. row count, lead rows, spark side. */
  std::string in_call_story_shape;
  bool in_call_story_sheet_open = false;
  std::vector<CallStoryLine> in_call_story_lines;
  std::string in_call_story_how;
  // Bound, part-dirtied (kCallPartVariables): the result line, the signal bar and the top pill.
  std::string in_call_result_text;
  bool in_call_result_fading = false;
  bool in_call_signal_shown = false;
  int in_call_signal_bars = 4;
  int in_call_signal_tone = 0;
  std::string in_call_signal_word;
  std::string in_call_notice_text;
  bool in_call_notice_warn = false;
  bool in_call_notice_fading = false;
  std::string in_call_reconnect_text;
  bool in_call_reconnect_ok = false;
  bool in_call_reconnect_fading = false;
  bool in_call_reconnect_dim = false;
};

enum class CallChromeUpdate {
  None,       // idle poll / no visible change
  DirtyOnly,  // subtitle/labels while layer already shown
  Remount,    // layer appear / disappear / switch call_id → RemountCallChrome mounts
  LevelsOnly, // only the audio meters changed while they are on screen → dirty kCallLevelVariables
  /** Only bound parts changed (the call clock, the signal, the result line, the top pill, the quality chip): each
   * changed variable is dirtied on its own (CallChromeChangedVariables), never the whole call group (B146). */
  PartsOnly,
};

/** Window-model variables behind the 1:1 audio meters; LevelsOnly dirties exactly these (B146). */
inline constexpr std::array<const char*, 4> kCallLevelVariables = {
    "call_in_progress_mic_level",
    "call_in_progress_peer_level",
    "call_in_progress_mic_hint",
    "call_in_progress_peer_hint",
};

/**
 * The window-model variables of the in-call chrome's bound parts whose value differs between `before` and `after`:
 * the meters, the call clock, the quality chip, the phone's result line, signal bar and top pill, and the roster arrays
 * whose speaking rings changed. LevelsOnly and PartsOnly dirty exactly these.
 */
std::vector<const char*> CallChromeChangedVariables(const CallInProgressState& before,
                                                    const CallInProgressState& after);

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
