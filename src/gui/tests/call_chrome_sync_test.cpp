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

// The awaiting page becomes the real ring in place: every difference between them is a bound value, so it is a dirty
// and never a remount (a remount flashed the page); the ring appearing from nothing still mounts.
TEST(CallChromeSyncTest, AwaitingRingBecomesTheRealRingByDirtyOnly) {
  pbr::CallChromeLayer awaiting;
  awaiting.ring_active = true;
  awaiting.ring_awaiting_invite = true;
  awaiting.ring_phone_title = "Incoming call, connecting…";
  pbr::CallChromeLayer real = awaiting;
  real.ring_awaiting_invite = false;
  real.ring_call_id = "c1";
  real.ring_phone_title = "Bob";
  real.ring_phone_subtitle = "PP voice call";
  real.ring_avatar = "photo|bob.png";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(awaiting, real), pbr::CallChromeUpdate::DirtyOnly);
  pbr::CallChromeLayer only_flag = awaiting;
  only_flag.ring_awaiting_invite = false;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(awaiting, only_flag), pbr::CallChromeUpdate::DirtyOnly);
  pbr::CallChromeLayer nothing;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(nothing, awaiting), pbr::CallChromeUpdate::Remount) << "appears from nothing";
  pbr::CallChromeLayer other = real;
  other.ring_call_id = "c2";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(real, other), pbr::CallChromeUpdate::Remount)
      << "another call: still a remount";
  pbr::CallChromeLayer paid = awaiting;
  paid.ring_show_pricing = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(awaiting, paid), pbr::CallChromeUpdate::Remount) << "pricing is structure";
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

// The clock ticking dirties the clock alone, not the whole call group (B146; the phone page idles between ticks).
TEST(CallChromeSyncTest, ElapsedSubtitleTicksArePartsOnly) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_subtitle = "0:01";
  pbr::CallChromeLayer next = synced;
  next.in_call_subtitle = "0:02";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);
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

TEST(CallChromeSyncTest, LevelAndTimerTogetherArePartsOnly) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_elapsed = "0:05";
  pbr::CallChromeLayer next = synced;
  next.in_call_elapsed = "0:06";
  next.in_call_mic_level = 2;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);
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

// #83: the flip button's lit state (front camera) only re-binds its class, it does not rebuild the page.
TEST(CallChromeSyncTest, CameraFacingDirtiesOnly) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_show_flip = true;
  synced.in_call_camera_front = true;
  pbr::CallChromeLayer next = synced;
  next.in_call_camera_front = false;
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

// #166: the chip's place is written into its markup (ShellCallChromeRml): a new side or a new height remounts.
TEST(CallChromeSyncTest, MinimizedPlaceRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mode = pbr::CallChromeMode::Minimized;
  synced.in_call_minimized_side = 0;
  synced.in_call_minimized_y_permille = 0;
  pbr::CallChromeLayer next = synced;
  next.in_call_minimized_side = 1;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount) << "first drop";

  synced = next;
  next.in_call_minimized_side = 2;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount) << "the other side";

  synced = next;
  next.in_call_minimized_y_permille = 640;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount) << "same side, new height";

  synced = next;
  next.in_call_minimized_tucked = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount) << "bar <-> handle (#189)";

  synced = next;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::None) << "unchanged";
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
  next.ring_conflict_hint = "Accepting ends your call with Bob.";
  next.ring_accept_label = "End & Accept";
  next.ring_decline_label = "Ignore";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, QualityChipIsPartsOnly) {
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
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);
}

// The phone page's signal bar, result line and top pill are bound parts: a change dirties only them.
TEST(CallChromeSyncTest, PhoneStoryPartsArePartsOnly) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.phone = true;
  synced.in_call_mode = pbr::CallChromeMode::Immersive;
  pbr::CallChromeLayer next = synced;
  next.in_call_signal_bars = 2;
  next.in_call_signal_tone = 1;
  next.in_call_signal_word = "Fair";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);
  next = synced;
  next.in_call_result_text = "Connected directly · connected in 0.9 s";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);
  next = synced;
  next.in_call_notice_text = "Found a closer way and moved to it";
  next.in_call_notice_fading = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);
}

// What the phone page writes into its markup remounts it: the story's steps going, the failure page, the sheet.
TEST(CallChromeSyncTest, PhoneMarkupChangesRemount) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.phone = true;
  synced.in_call_story_active = true;
  pbr::CallChromeLayer next = synced;
  next.in_call_story_active = false;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  next = synced;
  next.in_call_story_stage = 1;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  next = synced;
  next.in_call_phone_failed = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  next = synced;
  next.in_call_story_sheet_open = true;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  // A new timeline line remounts only while the sheet shows it.
  synced.in_call_story_sheet_open = true;
  next = synced;
  next.in_call_story_lines.push_back({.time = "1:05", .text = "Found a closer way", .secure = true});
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  synced.in_call_story_sheet_open = next.in_call_story_sheet_open = false;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::None);
}

// The phone page has no audio meters: a level change there pushes nothing.
TEST(CallChromeSyncTest, LevelChangeOnThePhonePageIsNone) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.phone = true;
  pbr::CallChromeLayer next = synced;
  next.in_call_mic_level = 3;
  next.in_call_mic_hint = "Speaking";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::None);
}

// A roster row's mute or camera change still reaches the screen now that the clock tick no longer dirties all.
TEST(CallChromeSyncTest, RosterRowChangeDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_roster = "Ana|11|--";
  pbr::CallChromeLayer next = synced;
  next.in_call_roster = "Ana|11|m-";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

// PartsOnly / LevelsOnly dirty exactly the variables whose value changed.
TEST(CallChromeSyncTest, ChangedVariablesAreOnlyTheOnesThatChanged) {
  pbr::CallInProgressState before;
  pbr::CallInProgressState after = before;
  EXPECT_TRUE(pbr::CallChromeChangedVariables(before, after).empty());
  after.signal_bars = 2;
  after.subtitle = "0:07";
  after.notice_fading = true;
  after.title = "not a part"; // labels go through DirtyOnly, never here
  const std::vector<const char*> changed = pbr::CallChromeChangedVariables(before, after);
  std::vector<std::string> names(changed.begin(), changed.end());
  EXPECT_EQ(names, (std::vector<std::string>{"call_in_progress_subtitle", "call_in_progress_signal_bars",
                                             "call_in_progress_notice_fading"}));
}

TEST(CallChromePortsTest, MuteClassifyNotifiesDirtyOnly) {
  // CallController path: classify mute → apply_snapshot(..., DirtyOnly), never DirtyWindow.
  std::vector<pbr::CallChromeUpdate> applied;
  pbr::ShellCallChromePorts ports;
  ports.apply_snapshot = [&](const pbr::CallChromeSnapshot&, pbr::CallChromeUpdate u) { applied.push_back(u); };

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

// V059: the spotlight (who, and fill / fit) changes the Immersive markup: a remount, like a mode change.
TEST(CallChromeSyncTest, ASpotlightChangeRemounts) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_mode = pbr::CallChromeMode::Immersive;
  pbr::CallChromeLayer next = synced;
  next.in_call_spotlight = "22";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
  synced = next;
  next.in_call_spotlight_fill = false;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

// The phone page's speaking rings are bound parts (B146): a ring going on or off dirties only the roster arrays
// whose rows changed, never the whole call group nor a remount.
TEST(CallChromeSyncTest, ASpeakingRingIsPartsOnlyAndDirtiesOnlyItsRows) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.phone = true;
  synced.in_call_show_roster = true;
  synced.in_call_mode = pbr::CallChromeMode::Immersive;
  synced.in_call_roster = "Ana|11|--;Bo|22|--";
  pbr::CallChromeLayer next = synced;
  next.in_call_speaking = "22;";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::PartsOnly);

  pbr::CallInProgressState before;
  before.roster.resize(2);
  before.roster[0].stream_id = "11";
  before.roster[1].stream_id = "22";
  before.spotlight_rows = {before.roster[1]};
  before.strip_rows = {before.roster[0]};
  pbr::CallInProgressState after = before;
  after.roster[1].speaking = true;
  after.spotlight_rows[0].speaking = true;
  const std::vector<const char*> changed = pbr::CallChromeChangedVariables(before, after);
  std::vector<std::string> names(changed.begin(), changed.end());
  EXPECT_EQ(names, (std::vector<std::string>{"call_in_progress_roster", "call_in_progress_spotlight_rows"}))
      << "the strip's rows did not change";
}
