#include "feature/ui/CallChromeSync.h"
#include "feature/ui/ShellCallChromePorts.h"

#include <gtest/gtest.h>

#include <vector>

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

TEST(CallChromeSyncTest, StatusSubtitleRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_subtitle = "Connecting…";
  pbr::CallChromeLayer next = synced;
  next.in_call_subtitle = "Connected";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, ElapsedSubtitleTicksDirtyOnly) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_subtitle = "0:01";
  pbr::CallChromeLayer next = synced;
  next.in_call_subtitle = "0:02";
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

TEST(CallChromeSyncTest, MuteSpeakerCameraDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_muted = false;
  synced.in_call_speaker_on = true;
  synced.in_call_camera_on = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_muted = true;
  next.in_call_speaker_on = false;
  next.in_call_camera_on = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, StagePresenceRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_stage_visible = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_stage_visible = true;
  next.in_call_local_preview = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, GroupRosterPresenceRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.in_call_show_roster = true;
  next.in_call_participant_count = 3;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, ModeChangeRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mode = pbr::CallChromeMode::Expanded;
  pbr::CallChromeLayer next = synced;
  next.in_call_mode = pbr::CallChromeMode::Immersive;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  next.in_call_mode = pbr::CallChromeMode::Minimized;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, MinimizedCornerRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mode = pbr::CallChromeMode::Minimized;
  synced.in_call_minimized_corner = 0;
  pbr::CallChromeLayer next = synced;
  next.in_call_minimized_corner = 2;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, MuteUnderSameModeDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mode = pbr::CallChromeMode::Immersive;
  synced.in_call_muted = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_muted = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, DefaultModeFromRoster) {
  EXPECT_EQ(pbr::DefaultCallChromeMode(false), pbr::CallChromeMode::Expanded);
  EXPECT_EQ(pbr::DefaultCallChromeMode(true), pbr::CallChromeMode::Immersive);
}

TEST(CallChromeSyncTest, RingConflictDirties) {
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

TEST(CallChromeSyncTest, QualityChipDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_quality_bars = 4;
  synced.in_call_quality_ok = true;
  pbr::CallChromeLayer next = synced;
  next.in_call_quality_bars = 1;
  next.in_call_quality_ok = false;
  next.in_call_quality_error = true;
  next.in_call_quality_label = "Poor";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromePortsTest, MuteClassifyNotifiesDirtyOnly) {
  // CallController path: classify mute → apply_chrome_update(DirtyOnly), never DirtyWindow.
  std::vector<pbr::CallChromeUpdate> applied;
  pbr::ShellCallChromePorts ports;
  ports.apply_chrome_update = [&](pbr::CallChromeUpdate u) { applied.push_back(u); };

  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_muted = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_muted = true;

  const pbr::CallChromeUpdate update = pbr::ClassifyCallChromeUpdate(synced, next);
  ASSERT_EQ(update, pbr::CallChromeUpdate::DirtyOnly);
  ASSERT_TRUE(ports.apply_chrome_update);
  ports.apply_chrome_update(update);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied[0], pbr::CallChromeUpdate::DirtyOnly);
}
