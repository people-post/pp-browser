#include "gui/CallChromeSync.h"

#include <cctype>

namespace pbr {
namespace {

/** Calling/Connecting/Connected vs elapsed timer — remount when status kind flips (Dirty can miss). */
bool CallChromeStatusLabelChanged(const std::string& synced, const std::string& next) {
  if (synced == next) {
    return false;
  }
  auto kind = [](const std::string& s) {
    if (s.empty()) {
      return 0;
    }
    // Elapsed "0:05" / "12:34"
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
      return 1;
    }
    return 2; // localized status
  };
  const int a = kind(synced);
  const int b = kind(next);
  if (a != b) {
    return true;
  }
  return a == 2; // status string changed (Connecting… → Connected)
}

/** A row's speaking ring differs (any row, or the rows themselves). */
bool SpeakingChanged(const std::vector<CallRosterParticipantState>& before,
                     const std::vector<CallRosterParticipantState>& after) {
  if (before.size() != after.size()) {
    return true;
  }
  for (size_t i = 0; i < before.size(); ++i) {
    if (before[i].speaking != after[i].speaking) {
      return true;
    }
  }
  return false;
}

} // namespace

CallChromeMode DefaultCallChromeMode(bool show_roster) {
  return show_roster ? CallChromeMode::Immersive : CallChromeMode::Expanded;
}

CallChromeUpdate ClassifyCallChromeUpdate(const CallChromeLayer& synced, const CallChromeLayer& next) {
  const bool layer_changed = synced.ring_active != next.ring_active || synced.in_call_active != next.in_call_active ||
                             synced.ring_call_id != next.ring_call_id || synced.in_call_id != next.in_call_id;
  if (layer_changed) {
    return CallChromeUpdate::Remount;
  }

  // Button *presence* via data-if still remounts (same class of issue as ring layer appear).
  // Icon toggles (muted / speaker_on / camera_on) use DirtyOnly — DataViewIf + MountInner
  // model flush keep those in sync.
  // Mode / the minimized chip's place change the markup tree → Remount (V031, #166).
  if (synced.in_call_show_speaker != next.in_call_show_speaker ||
      synced.in_call_show_camera != next.in_call_show_camera || synced.in_call_show_flip != next.in_call_show_flip ||
      synced.in_call_show_invite != next.in_call_show_invite ||
      synced.in_call_show_make_group != next.in_call_show_make_group ||
      synced.in_call_group_invite != next.in_call_group_invite || synced.in_call_guests != next.in_call_guests ||
      synced.ring_guest_hint != next.ring_guest_hint || synced.in_call_show_retry != next.in_call_show_retry ||
      synced.in_call_calling != next.in_call_calling || synced.in_call_show_roster != next.in_call_show_roster ||
      synced.in_call_stage_visible != next.in_call_stage_visible || synced.in_call_mode != next.in_call_mode ||
      synced.in_call_spotlight != next.in_call_spotlight ||
      synced.in_call_spotlight_fill != next.in_call_spotlight_fill ||
      synced.in_call_minimized_side != next.in_call_minimized_side ||
      synced.in_call_minimized_y_permille != next.in_call_minimized_y_permille ||
      synced.in_call_minimized_tucked != next.in_call_minimized_tucked ||
      synced.ring_show_pricing != next.ring_show_pricing ||
      synced.ring_accept_charge_enabled != next.ring_accept_charge_enabled) {
    return CallChromeUpdate::Remount;
  }

  // The phone page writes these into its markup (ShellCallChromeRml): the story's steps, the failure page, names and
  // avatars, and the sheet's lines while it is open.
  if (synced.phone != next.phone || synced.ring_phone_title != next.ring_phone_title ||
      synced.ring_phone_subtitle != next.ring_phone_subtitle || synced.ring_avatar != next.ring_avatar ||
      synced.ring_awaiting_invite != next.ring_awaiting_invite ||
      synced.in_call_phone_title != next.in_call_phone_title || synced.in_call_video != next.in_call_video ||
      synced.in_call_avatars != next.in_call_avatars || synced.in_call_phone_failed != next.in_call_phone_failed ||
      synced.in_call_failed_text != next.in_call_failed_text ||
      synced.in_call_story_active != next.in_call_story_active ||
      synced.in_call_story_stage != next.in_call_story_stage ||
      synced.in_call_story_steps != next.in_call_story_steps ||
      synced.in_call_story_sheet_open != next.in_call_story_sheet_open ||
      (next.in_call_story_sheet_open && (synced.in_call_story_lines != next.in_call_story_lines ||
                                         synced.in_call_story_how != next.in_call_story_how))) {
    return CallChromeUpdate::Remount;
  }

  // Status subtitle must remount: deferred RemountCallChrome + DirtyOnly can leave data-rml on a
  // pre-bind element while synced_chrome_ already advanced (dogfood: stuck Connecting…).
  if (CallChromeStatusLabelChanged(synced.in_call_subtitle, next.in_call_subtitle)) {
    return CallChromeUpdate::Remount;
  }

  const bool labels_changed =
      synced.in_call_status_hint != next.in_call_status_hint || synced.in_call_roster != next.in_call_roster ||
      synced.ring_caller_label != next.ring_caller_label || synced.ring_media_label != next.ring_media_label ||
      synced.in_call_title != next.in_call_title || synced.in_call_muted != next.in_call_muted ||
      synced.in_call_camera_on != next.in_call_camera_on || synced.in_call_camera_front != next.in_call_camera_front ||
      synced.in_call_speaker_on != next.in_call_speaker_on ||
      synced.in_call_remote_video != next.in_call_remote_video ||
      synced.in_call_local_preview != next.in_call_local_preview ||
      synced.in_call_peer_label != next.in_call_peer_label ||
      synced.in_call_remote_placeholder != next.in_call_remote_placeholder ||
      synced.in_call_participant_count != next.in_call_participant_count || synced.ring_pulse != next.ring_pulse ||
      synced.ring_conflict != next.ring_conflict || synced.ring_eyebrow != next.ring_eyebrow ||
      synced.ring_conflict_hint != next.ring_conflict_hint || synced.ring_accept_label != next.ring_accept_label ||
      synced.ring_voice_answer_label != next.ring_voice_answer_label ||
      synced.ring_decline_label != next.ring_decline_label || synced.ring_pricing_label != next.ring_pricing_label ||
      synced.ring_accept_charge_label != next.ring_accept_charge_label ||
      synced.ring_accept_charge_hint != next.ring_accept_charge_hint ||
      synced.ring_accept_short != next.ring_accept_short || synced.ring_voice_short != next.ring_voice_short;
  if (labels_changed) {
    return CallChromeUpdate::DirtyOnly; // dirties the meters too
  }

  // The clock ticking ("0:05" → "0:06", a status kind change remounted above), the quality chip, and the phone's
  // result line / signal bar / top pill / speaking rings: each changed variable alone (B146).
  const bool parts_changed =
      synced.in_call_subtitle != next.in_call_subtitle || synced.in_call_elapsed != next.in_call_elapsed ||
      synced.in_call_quality_bars != next.in_call_quality_bars ||
      synced.in_call_quality_ok != next.in_call_quality_ok ||
      synced.in_call_quality_warn != next.in_call_quality_warn ||
      synced.in_call_quality_error != next.in_call_quality_error ||
      synced.in_call_quality_label != next.in_call_quality_label ||
      synced.in_call_quality_hint != next.in_call_quality_hint ||
      synced.in_call_result_text != next.in_call_result_text ||
      synced.in_call_result_fading != next.in_call_result_fading ||
      synced.in_call_signal_shown != next.in_call_signal_shown ||
      synced.in_call_signal_bars != next.in_call_signal_bars ||
      synced.in_call_signal_tone != next.in_call_signal_tone ||
      synced.in_call_signal_word != next.in_call_signal_word ||
      synced.in_call_notice_text != next.in_call_notice_text ||
      synced.in_call_notice_warn != next.in_call_notice_warn ||
      synced.in_call_notice_fading != next.in_call_notice_fading ||
      synced.in_call_reconnect_text != next.in_call_reconnect_text ||
      synced.in_call_reconnect_ok != next.in_call_reconnect_ok ||
      synced.in_call_reconnect_fading != next.in_call_reconnect_fading ||
      synced.in_call_reconnect_dim != next.in_call_reconnect_dim ||
      synced.in_call_show_debug_subtitle != next.in_call_show_debug_subtitle ||
      synced.in_call_debug_subtitle != next.in_call_debug_subtitle || synced.in_call_speaking != next.in_call_speaking;
  if (parts_changed) {
    return CallChromeUpdate::PartsOnly; // the meters' variables go too when they changed
  }

  const bool levels_changed =
      synced.in_call_mic_level != next.in_call_mic_level || synced.in_call_peer_level != next.in_call_peer_level ||
      synced.in_call_mic_hint != next.in_call_mic_hint || synced.in_call_peer_hint != next.in_call_peer_hint;
  if (levels_changed) {
    // Meters exist only in the Expanded bar without a roster (ShellCallChromeRml): elsewhere a
    // level change is invisible, and pushing it re-laid the whole call screen every frame (B146).
    const bool meters_on_screen = next.in_call_active && next.in_call_mode == CallChromeMode::Expanded &&
                                  !next.in_call_show_roster && !next.phone; // the phone page has no meters
    return meters_on_screen ? CallChromeUpdate::LevelsOnly : CallChromeUpdate::None;
  }
  return CallChromeUpdate::None;
}

std::vector<const char*> CallChromeChangedVariables(const CallInProgressState& before,
                                                    const CallInProgressState& after) {
  std::vector<const char*> out;
  const auto add = [&out](const bool changed, const char* name) {
    if (changed) {
      out.push_back(name);
    }
  };
  add(before.mic_level != after.mic_level, "call_in_progress_mic_level");
  add(before.peer_level != after.peer_level, "call_in_progress_peer_level");
  add(before.mic_hint != after.mic_hint, "call_in_progress_mic_hint");
  add(before.peer_hint != after.peer_hint, "call_in_progress_peer_hint");
  add(before.subtitle != after.subtitle, "call_in_progress_subtitle");
  add(before.elapsed != after.elapsed, "call_in_progress_elapsed");
  add(before.quality_bars != after.quality_bars, "call_in_progress_quality_bars");
  add(before.quality_ok != after.quality_ok, "call_in_progress_quality_ok");
  add(before.quality_warn != after.quality_warn, "call_in_progress_quality_warn");
  add(before.quality_error != after.quality_error, "call_in_progress_quality_error");
  add(before.quality_label != after.quality_label, "call_in_progress_quality_label");
  add(before.quality_hint != after.quality_hint, "call_in_progress_quality_hint");
  add(before.result_text != after.result_text, "call_in_progress_result_text");
  add(before.result_fading != after.result_fading, "call_in_progress_result_fading");
  add(before.signal_shown != after.signal_shown, "call_in_progress_signal_shown");
  add(before.signal_bars != after.signal_bars, "call_in_progress_signal_bars");
  add(before.signal_tone != after.signal_tone, "call_in_progress_signal_tone");
  add(before.signal_word != after.signal_word, "call_in_progress_signal_word");
  add(before.notice_text != after.notice_text, "call_in_progress_notice_text");
  add(before.notice_warn != after.notice_warn, "call_in_progress_notice_warn");
  add(before.notice_fading != after.notice_fading, "call_in_progress_notice_fading");
  add(before.reconnect_text != after.reconnect_text, "call_in_progress_reconnect_text");
  add(before.reconnect_ok != after.reconnect_ok, "call_in_progress_reconnect_ok");
  add(before.reconnect_fading != after.reconnect_fading, "call_in_progress_reconnect_fading");
  add(before.reconnect_dim != after.reconnect_dim, "call_in_progress_reconnect_dim");
  add(before.show_debug_subtitle != after.show_debug_subtitle, "call_in_progress_show_debug_subtitle");
  add(before.debug_subtitle != after.debug_subtitle, "call_in_progress_debug_subtitle");
  // The phone page's speaking rings: only the arrays whose rows' ring changed.
  add(SpeakingChanged(before.roster, after.roster), "call_in_progress_roster");
  add(SpeakingChanged(before.spotlight_rows, after.spotlight_rows), "call_in_progress_spotlight_rows");
  add(SpeakingChanged(before.strip_rows, after.strip_rows), "call_in_progress_strip_rows");
  return out;
}

CallChromeUpdate CallChromeSyncGate::Decide(const CallChromeLayer& next, std::chrono::steady_clock::time_point now) {
  const CallChromeUpdate update = ClassifyCallChromeUpdate(synced_, next);
  if (update == CallChromeUpdate::LevelsOnly) {
    if (levels_pushed_ && now - last_levels_ < kLevelInterval) {
      return CallChromeUpdate::None; // keep synced_: the next poll pushes the newest level
    }
    last_levels_ = now;
    levels_pushed_ = true;
  }
  synced_ = next;
  return update;
}

} // namespace pbr
