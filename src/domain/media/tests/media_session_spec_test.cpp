#include "domain/media/CallMediaEngine.h"

#include <gtest/gtest.h>
#include <opus.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using Spec = CallMediaEngine::SessionSpec;

/** One real 20 ms Opus frame (silence) so inbound decode paths run. */
std::vector<uint8_t> OpusSilenceFrame() {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  EXPECT_EQ(err, OPUS_OK);
  std::vector<int16_t> pcm(960, 0);
  std::vector<unsigned char> out(4000);
  const int n = opus_encode(enc, pcm.data(), 960, out.data(), static_cast<int>(out.size()));
  opus_encoder_destroy(enc);
  EXPECT_GT(n, 0);
  return {out.begin(), out.begin() + std::max(n, 0)};
}

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

class MediaSessionSpecTest : public ::testing::Test {
protected:
  void SetUp() override { engine_.SetSkipDeviceOpenForTest(true); }
  void TearDown() override { engine_.Stop(); }

  CallMediaEngine::SfuSendFn CountingSend() {
    return [this](const CallMediaEngine::SfuPacket&) { sent_.fetch_add(1); };
  }
  void Deliver(uint32_t stream_id, uint32_t seq) {
    CallMediaEngine::SfuPacket pkt;
    pkt.stream_id = stream_id;
    pkt.channel_id = 0;
    pkt.seq = seq;
    pkt.payload = frame_;
    engine_.OnSfuPacket(pkt);
  }

  CallMediaEngine engine_;
  std::atomic<int> sent_{0};
  std::vector<uint8_t> frame_ = OpusSilenceFrame();
};

// Viewer shape: receive → decode → mix; the mic is never opened and nothing is sent.
TEST_F(MediaSessionSpecTest, PlaybackOnlyReceivesAndNeverSends) {
  ASSERT_TRUE(engine_.Start("session:view", Spec::PlaybackOnly(), {}));
  EXPECT_TRUE(engine_.IsActive());
  EXPECT_EQ(engine_.ActiveSpec(), Spec::PlaybackOnly());
  for (uint32_t seq = 1; seq <= 5; ++seq) {
    Deliver(42, seq);
  }
  const auto health = engine_.HealthSnapshot();
  EXPECT_EQ(health.stream_count, 1u);
  EXPECT_GE(health.rx_audio_frames, 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(sent_.load(), 0);
  EXPECT_EQ(engine_.HealthSnapshot().tx_audio_frames, 0u);
  EXPECT_FALSE(engine_.HasLocalCapture());
  EXPECT_FALSE(engine_.SetCameraEnabled(true)) << "camera needs a capturing session";
}

// Broadcaster shape: capture → encode → send; inbound packets are not decoded.
TEST_F(MediaSessionSpecTest, CaptureOnlySendsAndIgnoresInbound) {
  ASSERT_TRUE(engine_.Start("session:cast", Spec::CaptureOnly(), CountingSend()));
  EXPECT_TRUE(WaitFor([this] { return sent_.load() >= 3; }, std::chrono::seconds(2)))
      << "silence frames (no device in tests) are paced out";
  Deliver(42, 1);
  EXPECT_EQ(engine_.HealthSnapshot().stream_count, 0u) << "no playback half, no RX tracks";
}

// Calls: StartSfu is exactly duplex.
TEST_F(MediaSessionSpecTest, StartSfuIsDuplex) {
  ASSERT_TRUE(engine_.StartSfu("call:1", CountingSend()));
  EXPECT_EQ(engine_.ActiveSpec(), Spec::Duplex());
  EXPECT_TRUE(WaitFor([this] { return sent_.load() >= 3; }, std::chrono::seconds(2)));
  Deliver(7, 1);
  EXPECT_EQ(engine_.HealthSnapshot().stream_count, 1u);
}

// A muted call (or one without a mic) keeps sending frames of silence: the peer's "no media" check
// (TX-only escalation) counts frames, not audio level, so it never mistakes a muted user for a
// broken path.
TEST_F(MediaSessionSpecTest, MutedCallStillSendsFrames) {
  ASSERT_TRUE(engine_.StartSfu("call:muted", CountingSend()));
  engine_.SetMuted(true);
  const int before = sent_.load();
  EXPECT_TRUE(WaitFor([&] { return sent_.load() >= before + 3; }, std::chrono::seconds(2)));
}

TEST_F(MediaSessionSpecTest, InvalidSpecsAreRefused) {
  EXPECT_FALSE(engine_.Start("s", Spec{false, false}, CountingSend())) << "needs a half";
  EXPECT_FALSE(engine_.Start("s", Spec::CaptureOnly(), {})) << "capturing needs a send fn";
  EXPECT_FALSE(engine_.IsActive());
}

// A different spec for the same session id rebuilds instead of send-swapping.
TEST_F(MediaSessionSpecTest, ChangingSpecRebuildsSession) {
  ASSERT_TRUE(engine_.Start("s", Spec::PlaybackOnly(), {}));
  ASSERT_TRUE(engine_.Start("s", Spec::Duplex(), CountingSend()));
  EXPECT_EQ(engine_.ActiveSpec(), Spec::Duplex());
  EXPECT_TRUE(WaitFor([this] { return sent_.load() >= 1; }, std::chrono::seconds(2)));
  engine_.Stop();
  EXPECT_EQ(engine_.ActiveSpec(), Spec::Duplex()) << "idle reports duplex";
  EXPECT_FALSE(engine_.IsActive());
}

} // namespace
} // namespace pbr
