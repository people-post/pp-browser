#include "feature/presenters/CallMediaFrame.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// The call screen's media decisions (CallMediaFrame, CALLS.md "CallController / shell"): one tick's facts in, what the
// status line says, whether the call reconnects, what the video stage shows — decided without a GUI.

using Status = CallScreen::Status;
using LineKind = CallStatusLine::Kind;

constexpr int64_t kLiveSince = 100'000;

CallScreen InCallScreen(const Status status = Status::Connected, const bool video = false) {
  CallScreen s;
  s.kind = CallScreen::Kind::InCall;
  s.call_id = "call-1";
  s.peer_identity = "peer";
  s.video = video;
  s.status = status;
  s.media_connected = status == Status::Connected;
  s.was_connected = true;
  s.connected_at_ms = kLiveSince;
  return s;
}

/** A live 1:1 voice call's facts at `now_ms`. */
CallMediaFacts LiveFacts(const int64_t now_ms) {
  CallMediaFacts f;
  f.call_id = "call-1";
  f.now_ms = now_ms;
  f.video_allowed = false;
  f.media_chrome_live = true;
  f.media_status = CallMediaStatus::DirectLive;
  f.live_since_ms = kLiveSince;
  f.media_active = true;
  f.have_peer_row = true;
  f.encoder_available = true;
  f.path_allows_video = true;
  return f;
}

CallMediaHealthView Health(const char* path = "direct", const CallPathQuality quality = CallPathQuality::Good,
                           const CallAudioAsymmetry asymmetry = CallAudioAsymmetry::None) {
  CallMediaHealthView h;
  h.path_kind = path;
  h.quality = quality;
  h.asymmetry = asymmetry;
  return h;
}

// The mic is gone even after the engine's audio rebuild: the hint line says the other side cannot hear, and it goes
// by itself once captured audio is back.
TEST(CallMediaFrameTest, ALostMicIsShownUntilCapturedAudioIsBack) {
  CallMediaFrame frame;
  CallMediaFacts facts = LiveFacts(160'000);
  facts.engine.capture_lost = true;
  const CallMediaScreen& lost = frame.Step(InCallScreen(), facts, Health());
  EXPECT_EQ(lost.hint_key, "call.quality.hint.mic_lost");
  EXPECT_TRUE(lost.hint_shown);

  facts.now_ms = 161'000;
  facts.engine.capture_lost = false;
  const CallMediaScreen& back = frame.Step(InCallScreen(), facts, Health());
  EXPECT_TRUE(back.hint_key.empty());
  EXPECT_FALSE(back.hint_shown);
}

TEST(CallMediaFrameTest, ALiveCallShowsItsTimerWithThePathWord) {
  CallMediaFrame frame;
  const CallMediaScreen& v = frame.Step(InCallScreen(), LiveFacts(160'000), Health("circuit"));
  EXPECT_FALSE(v.media.reconnect);
  EXPECT_EQ(v.line.kind, LineKind::Timer);
  EXPECT_EQ(v.line.since_ms, kLiveSince) << "the call's timer, from when it first went live";
  EXPECT_EQ(v.line.key, "call.status.connected") << "said while the timer reads nothing";
  EXPECT_EQ(v.line.path_key, "call.details.path.circuit");
  EXPECT_EQ(v.timer_since_ms, kLiveSince);
  EXPECT_EQ(v.reconnect.kind, CallReconnectKind::None);
}

// k4 / #171: a lost path greys the timer (it runs on) and the capsule says "Reconnecting…", "They may be offline" after
// 20 s — the call itself is dropped only after its 30 s window, by the stack — and "Restored" when the line is back.
TEST(CallMediaFrameTest, ALostPathReconnectsWithTheTimerRunningThenRestores) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  frame.Step(InCallScreen(), f, Health());
  f.now_ms = 161'000;
  f.media_status = CallMediaStatus::Reconnecting;
  f.media_chrome_live = false;
  CallMediaScreen v = frame.Step(InCallScreen(Status::Reconnecting), f, Health());
  EXPECT_TRUE(v.media.path_lost);
  EXPECT_TRUE(v.media.reconnect);
  EXPECT_EQ(v.line.kind, LineKind::Timer);
  EXPECT_EQ(v.line.since_ms, kLiveSince);
  EXPECT_TRUE(v.line.key.empty()) << "no words over the greyed timer: the capsule says it";
  EXPECT_TRUE(v.line.path_key.empty()) << "no path word while reconnecting";
  EXPECT_EQ(v.reconnect.kind, CallReconnectKind::Reconnecting);
  EXPECT_TRUE(v.reconnect.timer_dim);

  f.now_ms = 161'000 + CallReconnectCapsule::kOfflineAfterMs;
  v = frame.Step(InCallScreen(Status::Reconnecting), f, Health());
  EXPECT_EQ(v.reconnect.kind, CallReconnectKind::Offline);

  f.now_ms += 1'000;
  f.media_status = CallMediaStatus::DirectLive;
  f.media_chrome_live = true;
  v = frame.Step(InCallScreen(), f, Health());
  EXPECT_FALSE(v.media.reconnect);
  EXPECT_EQ(v.reconnect.kind, CallReconnectKind::Restored);
}

TEST(CallMediaFrameTest, MediaThatStoppedWhileTheCallRunsIsAReconnect) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.media_chrome_live = false; // was live (live_since), the engine still runs
  const CallMediaScreen& v = frame.Step(InCallScreen(), f, Health());
  EXPECT_TRUE(v.media.media_dropped);
  EXPECT_TRUE(v.media.reconnect);
  EXPECT_EQ(v.line.kind, LineKind::Timer);
  EXPECT_TRUE(v.line.key.empty());
  EXPECT_EQ(v.reconnect.kind, CallReconnectKind::Reconnecting);
}

// V060: on hold the audio is silence by design — not a reconnect, no "no audio" hint; the line says why. Only a lost
// path still reconnects.
TEST(CallMediaFrameTest, OnHoldHereTheLineSaysSoAndSilenceIsNoReconnect) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.local_on_hold = true;
  f.media_chrome_live = false; // silence: the media reads dropped
  CallMediaScreen v = frame.Step(InCallScreen(), f, Health("direct", CallPathQuality::NoAudio));
  EXPECT_TRUE(v.media.on_hold);
  EXPECT_FALSE(v.media.reconnect);
  EXPECT_EQ(v.line.kind, LineKind::Words);
  EXPECT_EQ(v.line.key, "call.status.on_hold_local");
  EXPECT_TRUE(v.line.path_key.empty());

  f.media_status = CallMediaStatus::Reconnecting;
  v = frame.Step(InCallScreen(Status::Reconnecting), f, Health());
  EXPECT_TRUE(v.media.reconnect) << "a lost path is a reconnect, on hold or not";
  EXPECT_EQ(v.line.kind, LineKind::Timer);
}

TEST(CallMediaFrameTest, OnHoldThereTheLineNamesThemAndTheStageSaysNothingMore) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.peer_on_hold = true;
  const CallMediaScreen& v =
      frame.Step(InCallScreen(), f, Health("direct", CallPathQuality::Good, CallAudioAsymmetry::SendingOnly));
  EXPECT_EQ(v.line.kind, LineKind::PeerOnHold);
  EXPECT_EQ(v.line.key, "call.status.on_hold_peer_named");
  EXPECT_EQ(v.line.fallback_key, "call.status.on_hold_peer");
  EXPECT_FALSE(v.hint_key.empty()) << "the clock still says sending-only";
  EXPECT_FALSE(v.hint_shown) << "but the hint line does not, on hold";
  EXPECT_EQ(v.media.placeholder, CallStagePlaceholder::None) << "the pill and the line say it (#181)";
}

TEST(CallMediaFrameTest, APathThatCouldNotBeSetUpSaysCouldntConnectNotReconnecting) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.media_chrome_live = false;
  f.live_since_ms = 0;
  f.direct_connect_failed = true;
  CallScreen screen = InCallScreen(Status::Connecting);
  screen.connected_at_ms = 0;
  CallMediaScreen v = frame.Step(screen, f, Health());
  EXPECT_TRUE(v.media.couldnt_connect);
  EXPECT_FALSE(v.media.reconnect);
  EXPECT_EQ(v.line.key, "call.status.couldnt_connect");

  f.on_hop = true; // the group path carries it
  EXPECT_FALSE(frame.Step(screen, f, Health()).media.couldnt_connect);
  f.on_hop = false;
  screen.status = Status::Disconnected; // was live: "disconnected", decided by the screen
  v = frame.Step(screen, f, Health());
  EXPECT_FALSE(v.media.couldnt_connect);
  EXPECT_EQ(v.line.key, "call.status.disconnected");
}

TEST(CallMediaFrameTest, CouldntConnectExplainsItselfFromTheScreensHint) {
  CallMediaFrame frame;
  CallScreen screen = InCallScreen(Status::CouldntConnect);
  screen.hint = CallScreen::Hint::DirectConnect;
  CallMediaFacts f = LiveFacts(160'000);
  f.media_chrome_live = false;
  f.media_active = false;
  const CallMediaScreen& v = frame.Step(screen, f, Health());
  EXPECT_EQ(v.line.key, "call.status.couldnt_connect");
  EXPECT_EQ(v.line.hint, CallScreen::Hint::DirectConnect);
}

// #181: the hint line carries the sentence when there is one; else a broken line says its quality on the status line.
TEST(CallMediaFrameTest, ABrokenLineSaysItsQualityUnlessTheHintLineSaysIt) {
  CallMediaFrame frame;
  CallMediaScreen v = frame.Step(InCallScreen(), LiveFacts(160'000), Health("direct", CallPathQuality::NoAudio));
  EXPECT_EQ(v.line.key, CallPathQualityLabelKey(CallPathQuality::NoAudio));
  EXPECT_EQ(v.line.kind, LineKind::Words);
  EXPECT_EQ(v.line.path_key, "call.details.path.direct");

  v = frame.Step(InCallScreen(), LiveFacts(161'000),
                 Health("direct", CallPathQuality::Good, CallAudioAsymmetry::SendingOnly));
  EXPECT_EQ(v.line.kind, LineKind::Timer) << "the timer stays; the hint line says it";
  EXPECT_EQ(v.hint_key, "call.quality.hint.connecting_voice");
  EXPECT_TRUE(v.hint_shown);
}

TEST(CallMediaFrameTest, OneWayAudioSaysItIsRetryingAfterEightSeconds) {
  CallMediaFrame frame;
  const auto one_way = Health("direct", CallPathQuality::Good, CallAudioAsymmetry::SendingOnly);
  frame.Step(InCallScreen(), LiveFacts(160'000), one_way);
  EXPECT_EQ(frame.Step(InCallScreen(), LiveFacts(168'000), one_way).hint_key, "call.quality.hint.connecting_voice");
  EXPECT_EQ(frame.Step(InCallScreen(), LiveFacts(168'001), one_way).hint_key, "call.quality.hint.retrying_voice");
}

// The participant count is the group's line while its media is live (ShowsParticipantCount), the live timer included.
TEST(CallMediaFrameTest, ALiveGroupCallsLineCountsItsPeople) {
  CallMediaFrame frame;
  CallScreen screen = InCallScreen();
  screen.group = true;
  screen.members = {{.identity = "a"}, {.identity = "b"}, {.identity = "me", .is_local = true}};
  CallMediaFacts f = LiveFacts(160'000);
  f.have_peer_row = false;
  CallMediaScreen v = frame.Step(screen, f, Health("media_relay"));
  EXPECT_EQ(v.line.kind, LineKind::Participants);
  EXPECT_EQ(v.line.key, "call.participants_elapsed");
  EXPECT_EQ(v.line.count, 3);
  EXPECT_EQ(v.line.since_ms, kLiveSince);
  EXPECT_EQ(v.line.path_key, "call.details.path.media_relay");

  f.media_status = CallMediaStatus::Reconnecting;
  screen.status = Status::Reconnecting;
  v = frame.Step(screen, f, Health());
  EXPECT_EQ(v.line.kind, LineKind::Timer) << "a reconnect keeps its own line";
}

TEST(CallMediaFrameTest, AGroupWaitingAloneCountsDown) {
  CallMediaFrame frame;
  CallScreen screen = InCallScreen(Status::WaitingForOthers);
  screen.group = true;
  screen.alone_until_ms = 200'000;
  CallMediaFacts f = LiveFacts(160'000);
  f.media_chrome_live = false;
  f.media_active = false;
  const CallMediaScreen& v = frame.Step(screen, f, Health());
  EXPECT_EQ(v.line.kind, LineKind::Countdown);
  EXPECT_EQ(v.line.key, "call.status.waiting_for_others");
  EXPECT_EQ(v.line.until_ms, 200'000);
}

// Kenneth 2026-09-29: a video call shows its stage from the answer with "Starting video…" until the first frame, at most
// kVideoStartMs after connect, and never while it still rings out.
TEST(CallMediaFrameTest, AVideoCallSaysStartingVideoUntilTheFirstFrameAtMostTenSeconds) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.video_allowed = true;
  f.video_intent = true;
  f.ringing_out = true;
  EXPECT_FALSE(frame.Step(InCallScreen(Status::Calling, true), f, Health()).media.starting_video);

  f.ringing_out = false;
  f.media_chrome_live = false;
  CallMediaScreen v = frame.Step(InCallScreen(Status::Connecting, true), f, Health());
  EXPECT_TRUE(v.media.starting_video);
  EXPECT_TRUE(v.media.stage_visible);
  EXPECT_EQ(v.media.placeholder, CallStagePlaceholder::StartingVideo);

  f.media_chrome_live = true;
  f.now_ms = 170'000; // live from here: the window opens
  EXPECT_TRUE(frame.Step(InCallScreen(Status::Connected, true), f, Health()).media.starting_video);
  f.now_ms = 170'000 + CallMediaFrame::kVideoStartMs - 1;
  EXPECT_TRUE(frame.Step(InCallScreen(Status::Connected, true), f, Health()).media.starting_video);
  f.now_ms = 170'000 + CallMediaFrame::kVideoStartMs;
  v = frame.Step(InCallScreen(Status::Connected, true), f, Health());
  EXPECT_FALSE(v.media.starting_video);
  EXPECT_EQ(v.media.placeholder, CallStagePlaceholder::CameraOff) << "their roster still says off";

  CallMediaFrame first_frame;
  f.now_ms = 171'000;
  f.ever_had_remote_video = true;
  EXPECT_FALSE(first_frame.Step(InCallScreen(Status::Connected, true), f, Health()).media.starting_video);
}

// B58: frames beat the roster — live frames mean the camera is on whatever the row says; an off row with no frames
// drops the last picture and the stage says "Camera off" (or "Camera paused", V060).
TEST(CallMediaFrameTest, LiveFramesBeatARosterThatSaysTheCameraIsOff) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.has_remote_video = true;
  f.peer_frames_live = true;
  CallMediaScreen v = frame.Step(InCallScreen(), f, Health());
  EXPECT_TRUE(v.media.peer_camera_on);
  EXPECT_FALSE(v.media.clear_remote_video);
  EXPECT_TRUE(v.media.remote_video);
  EXPECT_TRUE(v.media.stage_visible);
  EXPECT_EQ(v.media.placeholder, CallStagePlaceholder::None);

  f.peer_frames_live = false;
  v = frame.Step(InCallScreen(), f, Health());
  EXPECT_TRUE(v.media.clear_remote_video);
  EXPECT_FALSE(v.media.remote_video);
  EXPECT_EQ(v.media.placeholder, CallStagePlaceholder::CameraOff);
  f.peer_video_paused = true;
  v = frame.Step(InCallScreen(), f, Health());
  EXPECT_EQ(v.media.placeholder, CallStagePlaceholder::CameraPaused);
  EXPECT_TRUE(v.media.stage_visible) << "a paused camera keeps its stage";
}

// Kenneth 2026-10-23: a voice call's reconnect must not open an empty "camera off" stage; a video call's keeps it.
TEST(CallMediaFrameTest, AReconnectKeepsTheStageOnlyWhenVideoWasInTheCall) {
  CallMediaFacts f = LiveFacts(160'000);
  f.media_status = CallMediaStatus::Reconnecting;
  f.peer_camera_on = false;
  CallMediaFrame voice;
  EXPECT_FALSE(voice.Step(InCallScreen(Status::Reconnecting), f, Health()).media.stage_visible);
  f.ever_had_remote_video = true;
  f.peer_camera_on = true;
  CallMediaFrame video;
  const CallMediaScreen& v = video.Step(InCallScreen(Status::Reconnecting, true), f, Health());
  EXPECT_TRUE(v.media.reconnect);
  EXPECT_TRUE(v.media.stage_visible);
}

// V035 / M4: only an explicit "no video" narrows the call, once — and a sending camera keeps its off button.
TEST(CallMediaFrameTest, ACallNarrowedToVoiceTurnsTheCameraOffOnce) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.camera_on = true;
  f.video_allowed.reset(); // not known: hands off
  CallMediaScreen v = frame.Step(InCallScreen(), f, Health());
  EXPECT_FALSE(v.media.narrow_camera);
  EXPECT_TRUE(v.media.show_camera) << "a camera that is on can always be turned off";

  f.video_allowed = false;
  EXPECT_TRUE(frame.Step(InCallScreen(), f, Health()).media.narrow_camera);
  EXPECT_FALSE(frame.Step(InCallScreen(), f, Health()).media.narrow_camera) << "once";

  f.video_allowed = true;
  EXPECT_FALSE(frame.Step(InCallScreen(), f, Health()).media.narrow_camera);
  f.video_allowed = false;
  EXPECT_TRUE(frame.Step(InCallScreen(), f, Health()).media.narrow_camera) << "a new narrowing";

  f.camera_on = false;
  f.video_allowed = false;
  EXPECT_FALSE(frame.Step(InCallScreen(), f, Health()).media.show_camera) << "no video allowed, camera off: no button";
}

TEST(CallMediaFrameTest, AnsweringStepsOnlyTheStoryFromYouAnswered) {
  CallMediaFrame frame;
  CallScreen ring;
  ring.kind = CallScreen::Kind::Answering;
  ring.ring_call_id = "call-2";
  const CallMediaScreen& v = frame.StepAnswering(ring, 50'000);
  EXPECT_FALSE(v.in_call);
  EXPECT_EQ(v.call_id, "call-2");
  EXPECT_TRUE(v.story.connecting);
  EXPECT_FALSE(v.story.outgoing);
  EXPECT_EQ(v.story.stage, CallConnectStory::kStageAnswered);
  EXPECT_EQ(v.reconnect.kind, CallReconnectKind::None);
}

TEST(CallMediaFrameTest, ResetStartsOver) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.media_status = CallMediaStatus::Reconnecting;
  frame.Step(InCallScreen(Status::Reconnecting), f, Health());
  frame.Reset();
  EXPECT_EQ(frame.View().reconnect.kind, CallReconnectKind::None);
  EXPECT_TRUE(frame.View().call_id.empty());
}

TEST(CallMediaFrameTest, TheFactsHealthSamplesTheLinkAndReconnectsWithTheDecision) {
  CallMediaFrame frame;
  CallMediaFacts f = LiveFacts(160'000);
  f.media_status = CallMediaStatus::Reconnecting;
  const CallMediaScreen& v = frame.Step(InCallScreen(Status::Reconnecting), f);
  EXPECT_EQ(v.health.quality, CallPathQuality::Reconnecting) << "the health hears the call reconnects";
}

} // namespace
} // namespace pbr
