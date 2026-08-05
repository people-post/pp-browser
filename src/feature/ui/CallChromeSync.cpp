#include "feature/ui/CallChromeSync.h"

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

} // namespace

CallChromeMode DefaultCallChromeMode(bool show_roster) {
  return show_roster ? CallChromeMode::Immersive : CallChromeMode::Expanded;
}

CallChromeUpdate ClassifyCallChromeUpdate(const CallChromeLayer& synced, const CallChromeLayer& next) {
  const bool layer_changed = synced.ring_active != next.ring_active ||
                             synced.in_call_active != next.in_call_active ||
                             synced.ring_call_id != next.ring_call_id ||
                             synced.in_call_id != next.in_call_id;
  if (layer_changed) {
    return CallChromeUpdate::Remount;
  }

  // Button *presence* via data-if still remounts (same class of issue as ring layer appear).
  // Icon toggles (muted / speaker_on / camera_on) use DirtyOnly — DataViewIf + MountInner
  // model flush keep those in sync.
  // Mode / minimized corner change the markup tree → Remount (V031).
  if (synced.in_call_show_speaker != next.in_call_show_speaker ||
      synced.in_call_show_invite != next.in_call_show_invite ||
      synced.in_call_show_retry != next.in_call_show_retry ||
      synced.in_call_show_roster != next.in_call_show_roster ||
      synced.in_call_stage_visible != next.in_call_stage_visible ||
      synced.in_call_mode != next.in_call_mode ||
      synced.in_call_minimized_corner != next.in_call_minimized_corner) {
    return CallChromeUpdate::Remount;
  }

  // Status subtitle must remount: deferred RemountCallChrome + DirtyOnly can leave data-rml on a
  // pre-bind element while synced_chrome_ already advanced (dogfood: stuck Connecting…).
  if (CallChromeStatusLabelChanged(synced.in_call_subtitle, next.in_call_subtitle)) {
    return CallChromeUpdate::Remount;
  }

  const bool labels_changed = synced.in_call_subtitle != next.in_call_subtitle ||
                              synced.in_call_status_hint != next.in_call_status_hint ||
                              synced.ring_caller_label != next.ring_caller_label ||
                              synced.ring_media_label != next.ring_media_label ||
                              synced.in_call_title != next.in_call_title ||
                              synced.in_call_mic_level != next.in_call_mic_level ||
                              synced.in_call_peer_level != next.in_call_peer_level ||
                              synced.in_call_mic_hint != next.in_call_mic_hint ||
                              synced.in_call_peer_hint != next.in_call_peer_hint ||
                              synced.in_call_muted != next.in_call_muted ||
                              synced.in_call_camera_on != next.in_call_camera_on ||
                              synced.in_call_speaker_on != next.in_call_speaker_on ||
                              synced.in_call_remote_video != next.in_call_remote_video ||
                              synced.in_call_local_preview != next.in_call_local_preview ||
                              synced.in_call_elapsed != next.in_call_elapsed ||
                              synced.in_call_peer_label != next.in_call_peer_label ||
                              synced.in_call_remote_placeholder != next.in_call_remote_placeholder ||
                              synced.in_call_participant_count != next.in_call_participant_count ||
                              synced.ring_pulse != next.ring_pulse ||
                              synced.ring_conflict != next.ring_conflict ||
                              synced.ring_eyebrow != next.ring_eyebrow ||
                              synced.ring_conflict_hint != next.ring_conflict_hint ||
                              synced.ring_accept_label != next.ring_accept_label ||
                              synced.ring_decline_label != next.ring_decline_label;
  if (labels_changed) {
    return CallChromeUpdate::DirtyOnly;
  }
  return CallChromeUpdate::None;
}

} // namespace pbr
