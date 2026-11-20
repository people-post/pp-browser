#include "lib/media/MediaEngine.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// M5 (final-review): SetSkipDeviceOpenForTest(true) must short-circuit OpenAudioDevices()
// before it ever touches VoiceProcessingIo::Open() or an SDL device (constructed here in
// capabilities/calls/src/lib/media/tests because pp_lib_media — and so pp_browser_media_test — already
// links MediaEngine.cpp; no real mic/speaker is opened).
TEST(MediaEngineHealthTest, SkipDeviceOpenReportsNoAudioIo) {
  MediaEngine engine;
  engine.SetSkipDeviceOpenForTest(true);

  auto started = engine.StartDuplex("skip-device-open-test-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);

  const auto health = engine.HealthSnapshot();
  EXPECT_EQ(health.audio_io, "none");
  EXPECT_EQ(health.io_underruns, 0u);
  // No video thread is running: no stage clock and no fetch in progress.
  EXPECT_EQ(health.video_stage, "idle");
  EXPECT_EQ(health.video_stage_ms, 0);
  EXPECT_EQ(health.video_fetch_ms, 0);

  engine.Stop();
}

// A frame fetch inside the capture stack for longer than 2 s is a stuck thread; 0 means "not inside one".
TEST(MediaEngineHealthTest, VideoThreadStuckIsAFetchPastTwoSeconds) {
  EXPECT_FALSE(VideoThreadStuck(0, "wait_frame", 9000)) << "not inside a fetch: a quiet camera";
  EXPECT_FALSE(VideoThreadStuck(1999, "wait_frame", 1999));
  EXPECT_FALSE(VideoThreadStuck(kVideoThreadStuckMs, "wait_frame", kVideoThreadStuckMs));
  EXPECT_TRUE(VideoThreadStuck(2001, "wait_frame", 2001));
  EXPECT_EQ(VideoThreadStuckMs(2001, "wait_frame", 2001), 2001);
}

// Opening or closing the camera device for more than 5 s is stuck as well; other stages are not judged by their clock.
TEST(MediaEngineHealthTest, VideoThreadStuckAlsoCoversOpeningAndClosingTheCamera) {
  for (const char* stage : {"open_camera", "close_camera"}) {
    EXPECT_FALSE(VideoThreadStuck(0, stage, 0));
    EXPECT_FALSE(VideoThreadStuck(0, stage, kVideoCameraStageStuckMs));
    EXPECT_TRUE(VideoThreadStuck(0, stage, kVideoCameraStageStuckMs + 1));
    EXPECT_EQ(VideoThreadStuckMs(0, stage, 7000), 7000);
  }
  EXPECT_FALSE(VideoThreadStuck(0, "encode", 60000));
  EXPECT_FALSE(VideoThreadStuck(0, "sleep", 60000));
  EXPECT_FALSE(VideoThreadStuck(0, "wait_camera", 60000));
}

TEST(MediaEngineHealthTest, VideoFieldsStartAtRest) {
  const MediaEngineHealth fresh;
  EXPECT_EQ(fresh.video_stage, "idle");
  EXPECT_EQ(fresh.video_stage_ms, 0);
  EXPECT_EQ(fresh.video_fetch_ms, 0);
}

// B233: "a microphone is wanted and no capture device is open" is true only when nothing else explains the silence.
TEST(MicWantedButNotOpenTest, HoldsOnlyWhenNothingElseExplainsTheSilence) {
  MicWantedInputs in;
  in.engine_running = true;
  in.capture_wanted = true;
  EXPECT_TRUE(MicWantedButNotOpen(in));

  MicWantedInputs not_running = in;
  not_running.engine_running = false;
  EXPECT_FALSE(MicWantedButNotOpen(not_running)) << "a stopped engine";
  MicWantedInputs no_capture = in;
  no_capture.capture_wanted = false;
  EXPECT_FALSE(MicWantedButNotOpen(no_capture)) << "a session that does not send audio";
  MicWantedInputs muted = in;
  muted.muted = true;
  EXPECT_FALSE(MicWantedButNotOpen(muted)) << "the user muted";
  MicWantedInputs open = in;
  open.capture_device_open = true;
  EXPECT_FALSE(MicWantedButNotOpen(open)) << "a device is open";
  MicWantedInputs denied = in;
  denied.permission_granted = false;
  EXPECT_FALSE(MicWantedButNotOpen(denied)) << "permission denied or not yet answered";
  MicWantedInputs interrupted = in;
  interrupted.os_has_session = true;
  EXPECT_FALSE(MicWantedButNotOpen(interrupted)) << "a phone call or Siri has the session";
  MicWantedInputs routing = in;
  routing.route_change_pending = true;
  EXPECT_FALSE(MicWantedButNotOpen(routing)) << "a route change is queued or running";
}

// The count is on the caller's monotonic clock: a fake one here. Any tick where the state does not hold starts it over.
TEST(MicMissingClockTest, CountsFromTheFirstReadingAndStartsOverWhenTheStateEnds) {
  MicMissingClock clock;
  auto r = clock.Step(false, false, 1000);
  EXPECT_EQ(r.missing_ms, 0);
  EXPECT_FALSE(r.began);
  EXPECT_FALSE(r.ever_open);

  r = clock.Step(true, false, 2000);
  EXPECT_TRUE(r.began);
  EXPECT_EQ(r.missing_ms, 1) << "never 0 while it holds";
  r = clock.Step(true, false, 4999);
  EXPECT_FALSE(r.began);
  EXPECT_EQ(r.missing_ms, 2999);
  r = clock.Step(true, false, 5000);
  EXPECT_EQ(r.missing_ms, 3000);

  r = clock.Step(false, true, 5600); // a device opened by itself
  EXPECT_EQ(r.missing_ms, 0);
  EXPECT_EQ(r.ended_after_ms, 3600);
  EXPECT_TRUE(r.ever_open);
  r = clock.Step(false, true, 5700);
  EXPECT_EQ(r.ended_after_ms, 0) << "the end is said once";
  EXPECT_TRUE(r.ever_open);

  r = clock.Step(true, false, 9000); // a new spell counts from its own start
  EXPECT_TRUE(r.began);
  EXPECT_EQ(r.missing_ms, 1);
}

TEST(MicMissingClockTest, AnExcludedStateInTheMiddleRestartsTheCount) {
  MicMissingClock clock;
  clock.Step(true, false, 1000);
  EXPECT_EQ(clock.Step(true, false, 3500).missing_ms, 2500);
  // The OS takes the session (a phone call): the state does not hold, so the count is dropped.
  EXPECT_EQ(clock.Step(false, false, 4000).missing_ms, 0);
  EXPECT_EQ(clock.Step(true, false, 6000).missing_ms, 1) << "counted again from when it holds again";
  EXPECT_FALSE(clock.Step(true, false, 6500).ever_open);
}

// B233: the engine opening or re-opening its own audio is not a missing microphone, for at most kMicOpenGraceMs.
TEST(MicMissingReportedTest, ASlowFirstOpenOfFourSecondsIsNotReported) {
  EXPECT_EQ(MicMissingReportedMs(4000, /*engine_opening=*/true), 0);
}

TEST(MicMissingReportedTest, ADefaultDeviceReopenOfAirPodsIsNotReported) {
  // The mic closed when the route moved and the reopen took 4.5 s: opening the whole time.
  EXPECT_EQ(MicMissingReportedMs(4500, /*engine_opening=*/true), 0);
  EXPECT_EQ(MicMissingReportedMs(kMicOpenGraceMs - 1, true), 0);
}

TEST(MicMissingReportedTest, AHungOpenOfTwelveSecondsIsReported) {
  EXPECT_EQ(MicMissingReportedMs(12'000, /*engine_opening=*/true), 12'000);
  EXPECT_EQ(MicMissingReportedMs(kMicOpenGraceMs, true), kMicOpenGraceMs);
}

TEST(MicMissingReportedTest, TheConfirmedFailureIsReportedAtOnceWhenTheOpenSequenceHasFinished) {
  // Lease present with no device, nothing opening any more (B233): the raw time is what the call grades on.
  EXPECT_EQ(MicMissingReportedMs(3000, /*engine_opening=*/false), 3000);
  EXPECT_EQ(MicMissingReportedMs(0, true), 0);
  EXPECT_EQ(MicMissingReportedMs(0, false), 0);
}

// Asking the engine for its health changes nothing and, muted, never reports a missing microphone (no permission query
// can matter: muted is excluded first).
TEST(MediaEngineHealthTest, AMutedEngineReportsNoMissingMicrophone) {
  MediaEngine engine;
  engine.SetSkipDeviceOpenForTest(true);
  auto started = engine.StartDuplex("mic-missing-muted-test-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);
  engine.SetMuted(true);
  EXPECT_EQ(engine.HealthSnapshot().mic_missing_ms, 0);
  EXPECT_EQ(engine.HealthSnapshot().mic_missing_ms, 0);
  engine.Stop();
}

} // namespace
} // namespace pbr
