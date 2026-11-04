#include "gui/CallChromeSync.h"
#include "gui/shell/ShellCallChromePorts.h"

#include <gtest/gtest.h>

#include <chrono>
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

TEST(CallChromeSyncTest, RingPricingPresenceRemounts) {
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.ring_show_pricing = true;
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

TEST(CallChromeSyncTest, MicLevelOnlyIsLevelsOnlyWhileMetersShow) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mic_level = 0;
  synced.in_call_mic_hint = "Silent";
  pbr::CallChromeLayer next = synced;
  next.in_call_mic_level = 3;
  next.in_call_mic_hint = "Speaking";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::LevelsOnly);
}

TEST(CallChromeSyncTest, LevelChangeWithoutMetersOnScreenIsNone) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.in_call_peer_level = 4;
  next.in_call_peer_hint = "Loud";

  synced.in_call_show_roster = next.in_call_show_roster = true; // group call
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::None);

  synced.in_call_show_roster = next.in_call_show_roster = false;
  synced.in_call_mode = next.in_call_mode = pbr::CallChromeMode::Immersive;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::None);

  synced.in_call_mode = next.in_call_mode = pbr::CallChromeMode::Minimized;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::None);
}

TEST(CallChromeSyncTest, LevelAndTimerTogetherDirtyEverything) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_elapsed = "0:05";
  pbr::CallChromeLayer next = synced;
  next.in_call_elapsed = "0:06";
  next.in_call_mic_level = 2;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

namespace {

pbr::CallChromeLayer OneToOneCall(int mic_level) {
  pbr::CallChromeLayer layer;
  layer.in_call_active = true;
  layer.in_call_id = "c1";
  layer.in_call_mic_level = mic_level;
  return layer;
}

} // namespace

TEST(CallChromeSyncGateTest, MetersArePushedAtMostEveryIntervalAndTheHeldLevelCatchesUp) {
  using std::chrono::milliseconds;
  const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  pbr::CallChromeSyncGate gate;
  ASSERT_EQ(gate.Decide(OneToOneCall(0), t0), pbr::CallChromeUpdate::Remount); // call appears

  EXPECT_EQ(gate.Decide(OneToOneCall(2), t0 + milliseconds(10)), pbr::CallChromeUpdate::LevelsOnly);
  // Inside the interval: held back.
  EXPECT_EQ(gate.Decide(OneToOneCall(4), t0 + milliseconds(100)), pbr::CallChromeUpdate::None);
  // The level then stays at 4; once the interval has passed the same poll pushes it.
  EXPECT_EQ(gate.Decide(OneToOneCall(4), t0 + milliseconds(150)), pbr::CallChromeUpdate::None);
  EXPECT_EQ(gate.Decide(OneToOneCall(4), t0 + milliseconds(260)), pbr::CallChromeUpdate::LevelsOnly);
  // Pushed: nothing left to do.
  EXPECT_EQ(gate.Decide(OneToOneCall(4), t0 + milliseconds(600)), pbr::CallChromeUpdate::None);
}

TEST(CallChromeSyncGateTest, OtherChangesAreNeverHeldBack) {
  const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  pbr::CallChromeSyncGate gate;
  gate.Decide(OneToOneCall(0), t0);
  ASSERT_EQ(gate.Decide(OneToOneCall(3), t0), pbr::CallChromeUpdate::LevelsOnly);
  pbr::CallChromeLayer muted = OneToOneCall(0);
  muted.in_call_muted = true;
  EXPECT_EQ(gate.Decide(muted, t0 + std::chrono::milliseconds(5)), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncGateTest, MetersComingBackOnScreenShowTheNewestLevel) {
  const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  pbr::CallChromeSyncGate gate;
  pbr::CallChromeLayer group = OneToOneCall(0);
  group.in_call_show_roster = true;
  gate.Decide(group, t0);
  for (int level = 1; level <= 5; ++level) {
    group.in_call_mic_level = level;
    EXPECT_EQ(gate.Decide(group, t0 + std::chrono::milliseconds(level)), pbr::CallChromeUpdate::None);
  }
  // Leaving the roster remounts the chrome, which binds the whole snapshot (level 5 included).
  EXPECT_EQ(gate.Decide(OneToOneCall(5), t0 + std::chrono::milliseconds(10)), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, SwitchCallIdRemounts) {
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.ring_call_id = "c2";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, CameraPresenceRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_show_camera = true;
  pbr::CallChromeLayer next = synced;
  next.in_call_show_camera = false;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, FlipPresenceRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.in_call_show_flip = true;
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
  // CallController path: classify mute → apply_snapshot(..., DirtyOnly), never DirtyWindow.
  std::vector<pbr::CallChromeUpdate> applied;
  pbr::ShellCallChromePorts ports;
  ports.apply_snapshot = [&](const pbr::CallChromeSnapshot&, pbr::CallChromeUpdate u) {
    applied.push_back(u);
  };

  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_muted = false;
  pbr::CallChromeLayer next = synced;
  next.in_call_muted = true;

  const pbr::CallChromeUpdate update = pbr::ClassifyCallChromeUpdate(synced, next);
  ASSERT_EQ(update, pbr::CallChromeUpdate::DirtyOnly);
  ASSERT_TRUE(ports.apply_snapshot);
  pbr::CallChromeSnapshot snap;
  snap.in_progress.active = true;
  snap.in_progress.muted = true;
  ports.apply_snapshot(snap, update);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied[0], pbr::CallChromeUpdate::DirtyOnly);
}
