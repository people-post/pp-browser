#include "common/ByteRateLimiter.h"
#include "domain/media/CallMediaAdaptation.h"
#include "domain/media/CallMediaPlayout.h"

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

AudioPacket Pkt(uint32_t seq, uint8_t fill = 1) {
  AudioPacket p;
  p.seq = seq;
  p.payload.assign(8, fill);
  return p;
}

TEST(AudioJitterBufferTest, PrimesThenPopsInOrder) {
  AudioJitterBuffer buf;
  EXPECT_EQ(buf.PopForPlayout().kind, AudioPlayoutPop::Kind::Empty);
  EXPECT_EQ(buf.underruns(), 0u); // not primed yet → not an underrun
  for (uint32_t i = 1; i <= AudioJitterBuffer::kTargetFrames; ++i) {
    buf.Push(Pkt(i, static_cast<uint8_t>(i)));
  }
  auto a = buf.PopForPlayout();
  ASSERT_EQ(a.kind, AudioPlayoutPop::Kind::Packet);
  EXPECT_EQ(a.seq, 1u);
  EXPECT_EQ(a.payload[0], 1);
  auto b = buf.PopForPlayout();
  EXPECT_EQ(b.seq, 2u);
}

TEST(AudioJitterBufferTest, ReordersBySeqAndDropsDuplicates) {
  AudioJitterBuffer buf;
  buf.Push(Pkt(3));
  buf.Push(Pkt(1));
  buf.Push(Pkt(2));
  buf.Push(Pkt(2)); // duplicate
  EXPECT_EQ(buf.size(), 3u);
  EXPECT_EQ(buf.PopForPlayout().seq, 1u);
  EXPECT_EQ(buf.PopForPlayout().seq, 2u);
  EXPECT_EQ(buf.PopForPlayout().seq, 3u);
}

TEST(AudioJitterBufferTest, GapReturnsNextPacketForFec) {
  AudioJitterBuffer buf;
  buf.Push(Pkt(1));
  buf.Push(Pkt(2));
  buf.Push(Pkt(4, 44)); // seq 3 missing
  EXPECT_EQ(buf.PopForPlayout().seq, 1u);
  EXPECT_EQ(buf.PopForPlayout().seq, 2u);
  auto gap = buf.PopForPlayout();
  ASSERT_EQ(gap.kind, AudioPlayoutPop::Kind::Gap);
  EXPECT_EQ(gap.seq, 3u);
  ASSERT_FALSE(gap.payload.empty());
  EXPECT_EQ(gap.payload[0], 44); // next packet's bytes, packet 4 stays queued
  EXPECT_EQ(buf.gaps(), 1u);
  auto four = buf.PopForPlayout();
  EXPECT_EQ(four.kind, AudioPlayoutPop::Kind::Packet);
  EXPECT_EQ(four.seq, 4u);
}

TEST(AudioJitterBufferTest, EmptyAfterPrimedCountsUnderrun) {
  AudioJitterBuffer buf;
  for (uint32_t i = 1; i <= AudioJitterBuffer::kTargetFrames; ++i) {
    buf.Push(Pkt(i));
  }
  for (uint32_t i = 1; i <= AudioJitterBuffer::kTargetFrames; ++i) {
    (void)buf.PopForPlayout();
  }
  EXPECT_EQ(buf.PopForPlayout().kind, AudioPlayoutPop::Kind::Empty);
  EXPECT_EQ(buf.underruns(), 1u);
}

TEST(AudioJitterBufferTest, DropsLatePacket) {
  AudioJitterBuffer buf;
  buf.Push(Pkt(1));
  buf.Push(Pkt(2));
  buf.Push(Pkt(4));
  (void)buf.PopForPlayout(); // 1
  (void)buf.PopForPlayout(); // 2
  (void)buf.PopForPlayout(); // gap for 3
  buf.Push(Pkt(3));          // arrives after its slot was already played
  EXPECT_EQ(buf.drops_late(), 1u);
  EXPECT_EQ(buf.PopForPlayout().seq, 4u);
}

TEST(AudioJitterBufferTest, DropsOverflowOldest) {
  AudioJitterBuffer buf;
  for (uint32_t i = 1; i <= AudioJitterBuffer::kMaxFrames + 3; ++i) {
    buf.Push(Pkt(i));
  }
  EXPECT_EQ(buf.size(), AudioJitterBuffer::kMaxFrames);
  EXPECT_EQ(buf.drops_overflow(), 3u);
  EXPECT_EQ(buf.PopForPlayout().seq, 4u); // oldest three dropped, playout resumes at 4
}

TEST(AudioJitterBufferTest, ResyncsOnLargeSeqJump) {
  AudioJitterBuffer buf;
  for (uint32_t i = 1; i <= AudioJitterBuffer::kTargetFrames; ++i) {
    buf.Push(Pkt(i));
  }
  for (uint32_t i = 1; i <= AudioJitterBuffer::kTargetFrames; ++i) {
    (void)buf.PopForPlayout();
  }
  // Sender restarted (SoftMigrate): seq jumps far ahead. Must not emit 1000 gaps.
  buf.Push(Pkt(1000));
  auto p = buf.PopForPlayout();
  EXPECT_EQ(p.kind, AudioPlayoutPop::Kind::Packet);
  EXPECT_EQ(p.seq, 1000u);
  EXPECT_EQ(buf.gaps(), 0u);
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
