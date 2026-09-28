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
  EXPECT_TRUE(gap.fec_usable); // packet 4's LBRR covers frame 3
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

using Kind = AudioPlayoutPop::Kind;
constexpr size_t kTarget = AudioJitterBuffer::kTargetFrames;

// Steady 1 push / 1 pop per slot until `slots` pops have happened; returns the next seq.
uint32_t RunSteady(AudioJitterBuffer& buf, uint32_t seq, int slots) {
  for (int i = 0; i < slots; ++i) {
    buf.Push(Pkt(seq++));
    (void)buf.PopForPlayout();
  }
  return seq;
}

TEST(AudioJitterBufferTest, PrimedOverflowResumesOnRealPackets) {
  AudioJitterBuffer buf;
  uint32_t seq = RunSteady(buf, 1, 20);
  // Device pause: packets keep arriving but nothing is popped.
  for (size_t i = 0; i < AudioJitterBuffer::kMaxFrames + 5; ++i) {
    buf.Push(Pkt(seq++));
  }
  EXPECT_EQ(buf.size(), AudioJitterBuffer::kMaxFrames);
  // Every pop after the pause is a real packet (no Gap for the overflow-dropped seqs), and the
  // surplus depth is drained within one full drain window (≤ 2 windows: the pause can land
  // mid-window, whose minimum still includes pre-pause depth).
  const int budget = static_cast<int>(2 * AudioJitterBuffer::kDrainWindowPops);
  for (int i = 0; i < budget + 50; ++i) {
    buf.Push(Pkt(seq++));
    const auto p = buf.PopForPlayout();
    ASSERT_EQ(p.kind, Kind::Packet) << "pop " << i;
    if (i >= budget) {
      EXPECT_LE(buf.size(), kTarget + 1) << "pop " << i;
    }
  }
  EXPECT_EQ(buf.gaps(), 0u);
}

void OutageRecovers(uint32_t lost) {
  SCOPED_TRACE(lost);
  AudioJitterBuffer buf;
  uint32_t seq = RunSteady(buf, 1, 100);
  const uint64_t gaps_before = buf.gaps();
  // `lost` consecutive packets never arrive; playout keeps ticking.
  for (uint32_t i = 0; i < lost; ++i) {
    ++seq;
    (void)buf.PopForPlayout();
  }
  int first_packet_slot = -1;
  std::vector<Kind> kinds;
  for (int slot = 0; slot < 300; ++slot) {
    buf.Push(Pkt(seq++));
    const auto p = buf.PopForPlayout();
    kinds.push_back(p.kind);
    if (p.kind == Kind::Packet && first_packet_slot < 0) {
      first_packet_slot = slot;
    }
  }
  ASSERT_GE(first_packet_slot, 0);
  EXPECT_LE(first_packet_slot, static_cast<int>(kTarget + 1));
  EXPECT_LE(buf.gaps() - gaps_before, kTarget);
  for (size_t i = kinds.size() - 50; i < kinds.size(); ++i) {
    EXPECT_EQ(kinds[i], Kind::Packet) << "slot " << i;
  }
  EXPECT_LE(buf.size(), kTarget + 1);
}

TEST(AudioJitterBufferTest, OutageOfManyPacketsRecoversQuickly) {
  OutageRecovers(12);
  OutageRecovers(25);
}

TEST(AudioJitterBufferTest, DelayedBurstRecovers) {
  AudioJitterBuffer buf;
  uint32_t seq = RunSteady(buf, 1, 100);
  std::vector<AudioPacket> held;
  const uint64_t underruns_before = buf.underruns();
  for (int i = 0; i < 20; ++i) {
    held.push_back(Pkt(seq++)); // delayed in the network
    (void)buf.PopForPlayout();  // plays what was buffered, then underruns
  }
  EXPECT_GE(buf.underruns() - underruns_before, 20u - kTarget);
  for (auto& p : held) {
    buf.Push(std::move(p));
  }
  std::vector<Kind> kinds;
  for (int slot = 0; slot < 200; ++slot) {
    buf.Push(Pkt(seq++));
    kinds.push_back(buf.PopForPlayout().kind);
  }
  EXPECT_LE(buf.size(), kTarget + 1);
  for (size_t i = kinds.size() - 50; i < kinds.size(); ++i) {
    EXPECT_EQ(kinds[i], Kind::Packet) << "slot " << i;
  }
}

TEST(AudioJitterBufferTest, SlightlyFastSenderNeverLatchesGaps) {
  AudioJitterBuffer buf;
  uint32_t seq = 1;
  for (int slot = 0; slot < 5000; ++slot) {
    buf.Push(Pkt(seq++));
    if (slot % 200 == 199) {
      buf.Push(Pkt(seq++));
    }
    const auto p = buf.PopForPlayout();
    if (slot >= static_cast<int>(kTarget)) {
      ASSERT_EQ(p.kind, Kind::Packet) << "slot " << slot;
    }
    ASSERT_LE(buf.size(), AudioJitterBuffer::kMaxFrames);
  }
  EXPECT_EQ(buf.gaps(), 0u);
}

TEST(AudioJitterBufferTest, BackwardSeqRestartReprimes) {
  AudioJitterBuffer buf;
  (void)RunSteady(buf, 1, 1000);
  const uint64_t late_before = buf.drops_late();
  // Peer re-StartSfu / BeginSession: seq restarts at 1 on the same track.
  uint32_t seq = 1;
  for (size_t i = 0; i < kTarget - 1; ++i) {
    buf.Push(Pkt(seq++));
    EXPECT_EQ(buf.PopForPlayout().kind, Kind::Empty); // re-priming
  }
  for (uint32_t expect = 1; expect <= 100; ++expect) {
    buf.Push(Pkt(seq++));
    const auto p = buf.PopForPlayout();
    ASSERT_EQ(p.kind, Kind::Packet);
    EXPECT_EQ(p.seq, expect);
  }
  EXPECT_EQ(buf.drops_late(), late_before);
}

TEST(AudioJitterBufferTest, EarlyBackwardRestartReprimesQuickly) {
  AudioJitterBuffer buf;
  (void)RunSteady(buf, 1, 30); // restart within the first kResyncJump packets of the call
  const uint64_t late_before = buf.drops_late();
  uint32_t seq = 1;
  int pops_until_audio = 0;
  for (; pops_until_audio < 20; ++pops_until_audio) {
    buf.Push(Pkt(seq++));
    const auto p = buf.PopForPlayout();
    if (p.kind == Kind::Packet && p.seq < 20) { // a packet of the restarted stream, not a leftover
      break;
    }
  }
  EXPECT_LE(pops_until_audio, 6); // ~120 ms of re-priming, not ~600 ms of dropped "late" packets
  EXPECT_LE(buf.drops_late() - late_before, 2u);
}

TEST(AudioJitterBufferTest, SmallBackwardStepIsStillLate) {
  AudioJitterBuffer buf;
  (void)RunSteady(buf, 1, 100);
  buf.Push(Pkt(90)); // reordered straggler, not a restart
  EXPECT_EQ(buf.drops_late(), 1u);
}

TEST(AudioJitterBufferTest, GapFecUsableOnlyForImmediatelyPrecedingFrame) {
  AudioJitterBuffer buf;
  buf.Push(Pkt(1));
  buf.Push(Pkt(2));
  buf.Push(Pkt(5, 55));
  auto a = buf.PopForPlayout();
  EXPECT_EQ(a.kind, Kind::Packet);
  EXPECT_EQ(a.seq, 1u);
  auto b = buf.PopForPlayout();
  EXPECT_EQ(b.kind, Kind::Packet);
  EXPECT_EQ(b.seq, 2u);
  auto g3 = buf.PopForPlayout();
  ASSERT_EQ(g3.kind, Kind::Gap);
  EXPECT_EQ(g3.seq, 3u);
  EXPECT_FALSE(g3.fec_usable); // packet 5's LBRR covers frame 4, not 3
  auto g4 = buf.PopForPlayout();
  ASSERT_EQ(g4.kind, Kind::Gap);
  EXPECT_EQ(g4.seq, 4u);
  EXPECT_TRUE(g4.fec_usable);
  ASSERT_FALSE(g4.payload.empty());
  EXPECT_EQ(g4.payload[0], 55);
  auto p5 = buf.PopForPlayout();
  EXPECT_EQ(p5.kind, Kind::Packet);
  EXPECT_EQ(p5.seq, 5u);
}

TEST(AudioJitterBufferTest, SmallHoleAfterUnderrunIsConcealed) {
  AudioJitterBuffer buf;
  for (uint32_t s = 1; s <= kTarget; ++s) {
    buf.Push(Pkt(s));
  }
  for (size_t i = 0; i < kTarget; ++i) {
    (void)buf.PopForPlayout();
  }
  EXPECT_EQ(buf.PopForPlayout().kind, Kind::Empty); // underrun, queue empty
  buf.Push(Pkt(kTarget + 2));                        // kTarget + 1 lost, lands in an empty queue
  const auto gap = buf.PopForPlayout();
  ASSERT_EQ(gap.kind, Kind::Gap);
  EXPECT_EQ(gap.seq, kTarget + 1);
  EXPECT_TRUE(gap.fec_usable);
  EXPECT_EQ(buf.PopForPlayout().seq, kTarget + 2);
}

TEST(AudioJitterBufferTest, HoleInDeepBufferIsSkipped) {
  AudioJitterBuffer buf;
  for (uint32_t s : {1u, 2u, 4u, 5u, 6u, 7u}) {
    buf.Push(Pkt(s));
  }
  EXPECT_EQ(buf.PopForPlayout().seq, 1u);
  EXPECT_EQ(buf.PopForPlayout().seq, 2u);
  // 3 missing but 4 packets are already buffered: skip rather than add a frame of latency.
  const auto p = buf.PopForPlayout();
  EXPECT_EQ(p.kind, Kind::Packet);
  EXPECT_EQ(p.seq, 4u);
  EXPECT_EQ(buf.gaps(), 0u);
}

TEST(AudioJitterBufferTest, SingleLossInNormalBufferIsConcealed) {
  AudioJitterBuffer buf;
  for (uint32_t s : {1u, 2u, 4u, 5u}) {
    buf.Push(Pkt(s));
  }
  EXPECT_EQ(buf.PopForPlayout().seq, 1u);
  EXPECT_EQ(buf.PopForPlayout().seq, 2u);
  // 3 missing but only 2 packets are buffered: conceal rather than skip.
  const auto gap = buf.PopForPlayout();
  ASSERT_EQ(gap.kind, Kind::Gap);
  EXPECT_EQ(gap.seq, 3u);
  EXPECT_TRUE(gap.fec_usable);
  const auto p = buf.PopForPlayout();
  EXPECT_EQ(p.kind, Kind::Packet);
  EXPECT_EQ(p.seq, 4u);
  EXPECT_EQ(buf.gaps(), 1u);
}

// Helper: feed `n` packets starting at `seq`, first arrival at `t0`, one every 20 ms plus `extra(i)` delay.
template <class Extra>
uint32_t FeedArrivals(AudioArrivalJitter& j, uint32_t seq, int64_t t0, int n, Extra extra) {
  for (int i = 0; i < n; ++i) {
    j.OnArrival(seq, t0 + static_cast<int64_t>(i) * 20 + extra(i));
    ++seq;
  }
  return seq;
}

// Every 100 packets (2 s) the network stalls 300 ms, then the backlog arrives at once.
int64_t StallExtra(int i) {
  const int phase = i % 100;
  return phase < 15 ? (15 - phase) * 20 : 0;  // packets 0..14 of each cycle arrive together at +300 ms
}

TEST(AudioArrivalJitterTest, WarmUpStartsAtMinimum) {
  AudioArrivalJitter j;
  EXPECT_EQ(j.TargetFrames(), 3u);
  FeedArrivals(j, 1, 1000, 5, [](int) { return 0; });
  EXPECT_EQ(j.TargetFrames(), 3u);
}

TEST(AudioArrivalJitterTest, SteadyNetworkStaysAtSixtyMs) {
  AudioArrivalJitter j;
  FeedArrivals(j, 1, 1000, 500, [](int i) { return (i % 3) * 2; });  // ±few ms of noise
  EXPECT_EQ(j.TargetFrames(), 3u);
}

TEST(AudioArrivalJitterTest, PeriodicStallsRaiseTarget) {
  AudioArrivalJitter j;
  FeedArrivals(j, 1, 1000, 1000, StallExtra);  // 20 s
  EXPECT_GE(j.TargetFrames(), 14u);           // >= 280 ms (99th pct of 15 staggered late packets per 100)
  EXPECT_LE(j.TargetFrames(), 20u);           // <= 400 ms
}

TEST(AudioArrivalJitterTest, TargetDecaysAfterNetworkCalms) {
  AudioArrivalJitter j;
  uint32_t seq = FeedArrivals(j, 1, 1000, 1000, StallExtra);
  FeedArrivals(j, seq, 1000 + 1000 * 20, 1500, [](int) { return 0; });  // 30 s calm
  EXPECT_LE(j.TargetFrames(), 5u);  // <= 100 ms
}

TEST(AudioArrivalJitterTest, SingleLatePacketDoesNotRaiseTarget) {
  AudioArrivalJitter j;
  FeedArrivals(j, 1, 1000, 500, [](int i) { return i == 250 ? 400 : 0; });
  EXPECT_EQ(j.TargetFrames(), 3u);
}

TEST(AudioArrivalJitterTest, LargeSeqValuesStayStable) {
  AudioArrivalJitter j;
  FeedArrivals(j, 0xFFFFFF00u, 5'000'000'000LL, 200, [](int) { return 0; });
  EXPECT_EQ(j.TargetFrames(), 3u);
}

TEST(AudioArrivalJitterTest, ReceiverClockStepRecovers) {
  AudioArrivalJitter j;
  uint32_t seq = FeedArrivals(j, 1, 100'000, 300, [](int) { return 0; });
  seq = FeedArrivals(j, seq, 50'000, 1500, [](int) { return 0; });  // clock stepped back 50 s, then steady 30 s
  EXPECT_LE(j.TargetFrames(), 5u);
}

TEST(AudioArrivalJitterTest, ResetForgetsHistory) {
  AudioArrivalJitter j;
  FeedArrivals(j, 1, 1000, 1000, StallExtra);
  j.Reset();
  EXPECT_EQ(j.TargetFrames(), 3u);
}

TEST(AudioArrivalJitterTest, HugeLatenessUsesOverflowBucket) {
  AudioArrivalJitter j;
  // Prime with 300 steady packets to establish baseline.
  FeedArrivals(j, 1, 1000, 300, [](int) { return 0; });
  // Every 10th packet arrives 2000 ms late; enough to exceed kMaxTargetFrames in raw form.
  FeedArrivals(j, 301, 1000 + 300 * 20, 100, [](int i) { return (i % 10 == 0) ? 2000 : 0; });
  // Overflow bucket clamp ensures target is clamped to kMaxTargetFrames (20), no crash.
  EXPECT_EQ(j.TargetFrames(), AudioArrivalJitter::kMaxTargetFrames);
}

// Push with realistic recv_ms so the estimator sees the network.
AudioPacket PktAt(uint32_t seq, int64_t recv_ms, uint8_t fill = 1) {
  AudioPacket p = Pkt(seq, fill);
  p.recv_ms = recv_ms;
  return p;
}

TEST(AudioJitterBufferTest, StallsRaiseTargetAndStopUnderruns) {
  AudioJitterBuffer buf;
  uint32_t seq = 1;
  uint64_t underruns_first_half = 0;
  for (int i = 0; i < 2000; ++i) {  // 40 s of 20 ms slots; StallExtra from Task 1 shapes arrivals
    const int64_t now = 1000 + static_cast<int64_t>(i) * 20;
    // deliver every packet whose (nominal + stall extra) arrival time has passed
    while (1000 + static_cast<int64_t>(seq - 1) * 20 + StallExtra(static_cast<int>(seq - 1)) <= now) {
      buf.Push(PktAt(seq, now));
      ++seq;
    }
    (void)buf.PopForPlayout();
    if (i == 999) underruns_first_half = buf.underruns();
  }
  EXPECT_GE(buf.TargetFrames(), 14u);
  // adapted: the last 20 s (10 stalls) underrun at most ~1 slot per stall (a fixed 60 ms target underruns ~12 per stall)
  EXPECT_LE(buf.underruns() - underruns_first_half, 10u);
}

TEST(AudioJitterBufferTest, RestartResetsJitterTarget) {
  AudioJitterBuffer buf;
  uint32_t seq = 1;
  for (int i = 0; i < 1000; ++i, ++seq) buf.Push(PktAt(seq, 1000 + i * 20 + StallExtra(i))), (void)buf.PopForPlayout();
  ASSERT_GE(buf.TargetFrames(), 14u);
  // Sender restart: large backward jump → Reset()
  for (uint32_t s = 1; s <= 5; ++s) buf.Push(PktAt(s, 100'000 + s * 20));
  EXPECT_EQ(buf.TargetFrames(), 3u);
}

TEST(AudioJitterBufferTest, OverTargetAndSilenceDropCounter) {
  AudioJitterBuffer buf;
  for (uint32_t s = 1; s <= 3; ++s) buf.Push(Pkt(s));
  EXPECT_FALSE(buf.OverTarget());   // 3 <= 3 + 1
  buf.Push(Pkt(4));
  buf.Push(Pkt(5));
  EXPECT_TRUE(buf.OverTarget());    // 5 > 4
  buf.NoteSilenceDrop();
  EXPECT_EQ(buf.silence_drops(), 1u);
  EXPECT_EQ(buf.speech_drops(), 0u);
}

TEST(AudioJitterBufferTest, SustainedSurplusIsTrimmedAsSpeech) {
  AudioJitterBuffer buf;
  uint32_t seq = 1;
  for (int i = 0; i < 12; ++i) buf.Push(Pkt(seq++));  // 12 queued, target 3 → surplus 9 > 5
  for (int i = 0; i < 30; ++i) {                       // steady 1-in-1-out for > one 25-pop window
    buf.Push(Pkt(seq++));
    (void)buf.PopForPlayout();
  }
  EXPECT_GT(buf.speech_drops(), 0u);
  EXPECT_LE(buf.size(), buf.TargetFrames() + 2);
}

TEST(AudioJitterBufferTest, HardCapIsFortyFrames) {
  AudioJitterBuffer buf;
  for (uint32_t s = 1; s <= 60; ++s) buf.Push(Pkt(s));
  EXPECT_LE(buf.size(), 40u);
  EXPECT_EQ(buf.speech_drops(), 20u);
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


TEST(ApplySoftGainTest, QuietSamplesGetFullGain) {
  std::vector<int16_t> pcm = {1000, -1000, 0, 5000};
  ApplySoftGain(pcm, 2.0f);
  EXPECT_NEAR(pcm[0], 2000, 2);
  EXPECT_NEAR(pcm[1], -2000, 2);
  EXPECT_EQ(pcm[2], 0);
  EXPECT_NEAR(pcm[3], 10000, 2);
}

TEST(ApplySoftGainTest, LoudSamplesNeverClipAndStayMonotonic) {
  std::vector<int16_t> pcm;
  for (int v = 0; v <= 32767; v += 64) {
    pcm.push_back(static_cast<int16_t>(v));
  }
  pcm.push_back(32767);
  ApplySoftGain(pcm, 2.0f);
  for (size_t i = 1; i < pcm.size(); ++i) {
    EXPECT_GE(pcm[i], pcm[i - 1]) << "at " << i;
  }
  EXPECT_LE(pcm.back(), 32767);
  EXPECT_GT(pcm.back(), 29000);  // a boosted full-scale peak lands near, not past, full scale
}

TEST(ApplySoftGainTest, SymmetricForNegativeSamples) {
  std::vector<int16_t> pos = {3000, 20000, 32767};
  std::vector<int16_t> neg = {-3000, -20000, -32767};
  ApplySoftGain(pos, 2.0f);
  ApplySoftGain(neg, 2.0f);
  for (size_t i = 0; i < pos.size(); ++i) {
    EXPECT_NEAR(pos[i], -neg[i], 1);
  }
}

} // namespace
} // namespace pbr
