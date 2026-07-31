#include "feature/ui/CallChromeSync.h"

#include <gtest/gtest.h>

TEST(CallChromeSyncTest, IdlePollUnchangedIsNone) {
  pbr::CallChromeLayer layer;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(layer, layer), pbr::CallChromeUpdate::None);
}

TEST(CallChromeSyncTest, RingAppearRemounts) {
  pbr::CallChromeLayer synced;
  pbr::CallChromeLayer next;
  next.ring_active = true;
  next.ring_call_id = "c1";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, RingDisappearRemounts) {
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  pbr::CallChromeLayer next;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, SubtitleOnlyDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_subtitle = "Connecting…";
  pbr::CallChromeLayer next = synced;
  next.in_call_subtitle = "Connected";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, MicLevelOnlyDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mic_level = 0;
  synced.in_call_mic_hint = "Silent";
  pbr::CallChromeLayer next = synced;
  next.in_call_mic_level = 3;
  next.in_call_mic_hint = "Speaking";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, SwitchCallIdRemounts) {
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.ring_call_id = "c2";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, MuteElapsedPulseDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_muted = false;
  synced.in_call_elapsed = "0:01";
  synced.in_call_peer_label = "Them";
  synced.ring_pulse = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_muted = true;
  next.in_call_elapsed = "0:02";
  next.in_call_peer_label = "Alice";
  next.ring_pulse = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, CameraStageDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_camera_on = false;
  synced.in_call_stage_visible = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_camera_on = true;
  next.in_call_stage_visible = true;
  next.in_call_local_preview = true;
  next.in_call_remote_placeholder = "Camera off";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, GroupRosterFieldsDirty) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.in_call_show_roster = true;
  next.in_call_participant_count = 3;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  synced.ring_conflict = false;
  synced.ring_accept_label = "Accept";
  synced.ring_decline_label = "Decline";
  pbr::CallChromeLayer next = synced;
  next.ring_conflict = true;
  next.ring_eyebrow = "You're already calling";
  next.ring_conflict_hint = "Alice is calling you back. Answering ends your outgoing call.";
  next.ring_accept_label = "End & Accept";
  next.ring_decline_label = "Ignore";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}
