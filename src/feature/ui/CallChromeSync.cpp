#include "feature/ui/CallChromeSync.h"

namespace pbr {

CallChromeUpdate ClassifyCallChromeUpdate(const CallChromeLayer& synced, const CallChromeLayer& next) {
  const bool layer_changed = synced.ring_active != next.ring_active ||
                             synced.in_call_active != next.in_call_active ||
                             synced.ring_call_id != next.ring_call_id ||
                             synced.in_call_id != next.in_call_id;
  if (layer_changed) {
    return CallChromeUpdate::Remount;
  }

  const bool labels_changed = synced.in_call_subtitle != next.in_call_subtitle ||
                              synced.ring_caller_label != next.ring_caller_label ||
                              synced.ring_media_label != next.ring_media_label ||
                              synced.in_call_title != next.in_call_title ||
                              synced.in_call_mic_level != next.in_call_mic_level ||
                              synced.in_call_peer_level != next.in_call_peer_level ||
                              synced.in_call_mic_hint != next.in_call_mic_hint ||
                              synced.in_call_peer_hint != next.in_call_peer_hint ||
                              synced.in_call_muted != next.in_call_muted ||
                              synced.in_call_elapsed != next.in_call_elapsed ||
                              synced.in_call_peer_label != next.in_call_peer_label ||
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
