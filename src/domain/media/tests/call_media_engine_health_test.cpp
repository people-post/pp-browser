#include "domain/media/CallMediaEngine.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// M5 (final-review): SetSkipDeviceOpenForTest(true) must short-circuit OpenAudioDevices()
// before it ever touches VoiceProcessingIo::Open() or an SDL device (constructed here in
// src/domain/media/tests because pp_domain_media — and so pp_browser_media_test — already
// links CallMediaEngine.cpp; no real mic/speaker is opened).
TEST(CallMediaEngineHealthTest, SkipDeviceOpenReportsNoAudioIo) {
  CallMediaEngine engine;
  engine.SetSkipDeviceOpenForTest(true);

  auto started = engine.StartSfu("skip-device-open-test-call", [](const CallMediaEngine::SfuPacket&) {});
  ASSERT_TRUE(started) << (started ? "" : started.error().message);

  const auto health = engine.HealthSnapshot();
  EXPECT_EQ(health.audio_io, "none");
  EXPECT_EQ(health.io_underruns, 0u);

  engine.Stop();
}

}  // namespace
}  // namespace pbr
