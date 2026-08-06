#include "base/media/ByteRateLimiter.h"
#include "base/media/CallMediaAdaptation.h"
#include "base/media/CallMediaPlayout.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

TEST(ByteRateLimiterTest, UnboundedAlwaysAllows) {
  ByteRateLimiter lim;
  lim.Configure(0);
  EXPECT_TRUE(lim.TryConsume(1'000'000, 1000));
}

TEST(ByteRateLimiterTest, EnforcesRate) {
  ByteRateLimiter lim;
  lim.Configure(/*rate_bps=*/8'000, /*burst_bytes=*/100); // 1000 bytes/s
  EXPECT_TRUE(lim.TryConsume(100, 0));
  EXPECT_FALSE(lim.TryConsume(100, 0)); // burst spent
  EXPECT_TRUE(lim.TryConsume(50, 1000)); // +1000ms → +1000 bytes tokens, capped at burst 100
}

TEST(AudioJitterBufferTest, PrimesThenPops) {
  AudioJitterBuffer buf;
  EXPECT_FALSE(buf.PopForPlayout(false).has_value());
  for (uint32_t i = 1; i <= AudioJitterBuffer::kTargetFrames; ++i) {
    PlayoutPcmFrame f;
    f.seq = i;
    f.pcm.assign(10, static_cast<int16_t>(i));
    buf.Push(std::move(f));
  }
  auto first = buf.PopForPlayout(false);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->seq, 1u);
}

TEST(AudioJitterBufferTest, DropsOverflowOldest) {
  AudioJitterBuffer buf;
  for (uint32_t i = 1; i <= AudioJitterBuffer::kMaxFrames + 3; ++i) {
    PlayoutPcmFrame f;
    f.seq = i;
    f.pcm.assign(4, 1);
    buf.Push(std::move(f));
  }
  EXPECT_EQ(buf.size(), AudioJitterBuffer::kMaxFrames);
  EXPECT_GT(buf.drops_overflow(), 0u);
}

TEST(MixPcmSatTest, Saturates) {
  std::vector<int16_t> out = {30000, -30000};
  std::vector<int16_t> in = {10000, -10000};
  MixPcmSat(out, in);
  EXPECT_EQ(out[0], 32767);
  EXPECT_EQ(out[1], -32768);
}

TEST(CallMediaAdaptationTest, PressureLowersAudioBps) {
  EXPECT_EQ(CallMediaAdaptation::AudioBpsForPressure(0.0), CallMediaAdaptation::kComfortAudioBps);
  EXPECT_EQ(CallMediaAdaptation::AudioBpsForPressure(1.0), CallMediaAdaptation::kMinAudioBps);
  EXPECT_LT(CallMediaAdaptation::AudioBpsForPressure(0.5), CallMediaAdaptation::kComfortAudioBps);
  CallAdaptationInput in;
  in.path_pressure = 0.9;
  in.per_user_up_bps = 2'000'000;
  const auto d = CallMediaAdaptation::Evaluate(in);
  EXPECT_TRUE(d.publish_audio);
  EXPECT_LE(d.target_audio_bps, CallMediaAdaptation::kComfortAudioBps);
  EXPECT_GE(d.target_audio_bps, CallMediaAdaptation::kMinAudioBps);
}

} // namespace
} // namespace pbr
