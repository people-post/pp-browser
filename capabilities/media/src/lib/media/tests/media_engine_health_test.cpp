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

} // namespace
} // namespace pbr
