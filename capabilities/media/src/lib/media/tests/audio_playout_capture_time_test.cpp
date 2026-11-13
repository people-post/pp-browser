// V066: the jitter buffer schedules by the sender's capture time; seq is for loss, order, FEC and duplicates only.
// A listener plays one publisher's 40 ms packets (what a call sends) in 20 ms slots, as the media engine does.
#include "lib/media/AudioPlayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace pbr {
namespace {

constexpr int kPacketMs = 40;
constexpr int kSlotMs = 20;
/** One way through the network, on top of the packet's own 40 ms (it is sent once its last sample is in). */
constexpr int64_t kNetMs = 30;
constexpr uint8_t kSilence = 0;
constexpr uint8_t kSpeech = 1;

/** A sent packet: its seq, its capture time (ms on the sender's clock), when it reaches the receiver, what it holds. */
struct Sent {
  uint32_t seq = 0;
  int64_t capture_ms = 0;
  int64_t recv_ms = 0;
  uint8_t fill = kSpeech;
  uint64_t instance = 1;
};

AudioPacket ToPacket(const Sent& sent) {
  AudioPacket p;
  p.seq = sent.seq;
  p.capture_ts = static_cast<uint32_t>(sent.capture_ms); // the wire's u32, wrapping
  p.instance = sent.instance;
  p.recv_ms = sent.recv_ms;
  p.duration_ms = kPacketMs;
  p.payload.assign(8, sent.fill);
  return p;
}

/** A packet the listener started to play: which, and when (receiver ms), and when it had arrived. */
struct Played {
  uint32_t seq = 0;
  int64_t at_ms = 0;
};

/**
 * The engine's playout of one track (MediaEngine::DecodeNextPacketLocked) in 20 ms slots: a packet or a concealed
 * one plays over its length, an Empty pop over one slot. Above the target a silent packet is skipped (one per pop, or
 * as many as the excess over the cap); over the cap with no silence left, speech is cut to it.
 */
class Listener {
public:
  /** Deliver `wire` (in arrival order) and play, slot by slot, until `until_ms`. */
  void Run(const std::vector<Sent>& wire, const int64_t until_ms) {
    for (const Sent& s : wire) {
      pending_.push_back(s);
    }
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Sent& a, const Sent& b) { return a.recv_ms < b.recv_ms; });
    for (; now_ms_ < until_ms; now_ms_ += kSlotMs) {
      while (!pending_.empty() && pending_.front().recv_ms <= now_ms_) {
        buf.Push(ToPacket(pending_.front()));
        pending_.pop_front();
      }
      Slot();
      max_depth_ms = std::max(max_depth_ms, buf.DepthMs());
    }
  }

  /** When `seq` started to play, if it did. */
  std::optional<int64_t> PlayedAt(const uint32_t seq) const {
    for (const Played& p : played) {
      if (p.seq == seq) {
        return p.at_ms;
      }
    }
    return std::nullopt;
  }
  /** Seqs in [from, to] that never played. */
  size_t MissingBetween(const uint32_t from, const uint32_t to) const {
    size_t missing = 0;
    for (uint32_t seq = from; seq <= to; ++seq) {
      missing += PlayedAt(seq) ? 0 : 1;
    }
    return missing;
  }

  AudioJitterBuffer buf;
  std::vector<Played> played;
  std::vector<AudioPlayoutPop> concealed; // Gap pops
  /** Underruns while packets were still to come (the stream running dry after its last packet is not one). */
  uint64_t underrun_slots = 0;
  int64_t max_depth_ms = 0;

private:
  void Slot() {
    if (left_ms_ <= 0) {
      Pop();
    }
    left_ms_ -= kSlotMs;
  }

  void Pop() {
    const size_t skips = buf.ExcessOverCap() + 1;
    for (size_t pass = 0;; ++pass) {
      const uint64_t underruns_before = buf.underruns();
      AudioPlayoutPop pop = buf.PopForPlayout();
      if (pop.kind == AudioPlayoutPop::Kind::Packet && pass < skips && buf.OverTarget() &&
          pop.payload.front() == kSilence) {
        buf.NoteSilenceDrop();
        continue;
      }
      switch (pop.kind) {
        case AudioPlayoutPop::Kind::Packet:
          played.push_back({pop.seq, now_ms_});
          left_ms_ = buf.PacketMs();
          break;
        case AudioPlayoutPop::Kind::Gap:
          concealed.push_back(pop);
          left_ms_ = buf.PacketMs();
          break;
        case AudioPlayoutPop::Kind::Empty:
          if (!pending_.empty()) {
            underrun_slots += buf.underruns() - underruns_before; // the stream's own, not its end running dry
          }
          left_ms_ = kSlotMs;
          break;
      }
      break;
    }
    buf.CutToCap();
  }

  std::deque<Sent> pending_;
  int64_t now_ms_ = 0;
  int left_ms_ = 0;
};

/** Seqs from..to of a steady 40 ms sender, capture time `capture(seq)`, arriving its packet length + kNetMs later. */
std::vector<Sent> Steady(const uint32_t from, const uint32_t to, const std::function<int64_t(uint32_t)>& capture) {
  std::vector<Sent> out;
  for (uint32_t seq = from; seq <= to; ++seq) {
    const int64_t at = capture(seq);
    out.push_back({.seq = seq, .capture_ms = at, .recv_ms = at + kPacketMs + kNetMs});
  }
  return out;
}

int64_t Delay(const Listener& ear, const std::vector<Sent>& wire, const uint32_t seq) {
  const Sent& sent = wire.at(seq - wire.front().seq);
  return *ear.PlayedAt(seq) - sent.recv_ms;
}

// #605: a sender's audio pauses (a Siri interruption or a hold, a speaker toggle, a mute) and then goes on at the
// nominal rate: its seq takes up where it stopped, its capture time has moved on by the pause. The packet that ends
// the pause plays on time — the target's delay after it arrives, as before the pause — and every packet after it
// plays: none is late, the receiver never needs a redial. (Seq as a clock dropped every one of them, forever.)
class SenderPause : public ::testing::TestWithParam<int64_t> {};

TEST_P(SenderPause, AudioResumesOnTimeAndNothingAfterIsLate) {
  const int64_t pause_ms = GetParam();
  Listener ear;
  const auto capture = [&](uint32_t seq) { return seq * kPacketMs + (seq > 250 ? pause_ms : 0); };
  const std::vector<Sent> wire = Steady(1, 500, capture); // 10 s, the pause, 10 s
  ear.Run(wire, wire.back().recv_ms + 500);

  const int64_t delay_before = Delay(ear, wire, 250);
  ASSERT_LE(delay_before, ear.buf.TargetMs() + kPacketMs);
  ASSERT_TRUE(ear.PlayedAt(251)) << "the first packet after the pause never played";
  EXPECT_LE(Delay(ear, wire, 251), delay_before + kPacketMs) << "it plays on time, within a packet";
  EXPECT_EQ(ear.MissingBetween(251, 500), 0u);
  EXPECT_EQ(ear.buf.drops_late(), 0u);
  EXPECT_LE(Delay(ear, wire, 400), delay_before + kSlotMs) << "and the stream after it plays as before the pause";
  EXPECT_EQ(ear.concealed.size(), 0u) << "a pause loses nothing: no Gap";
}

INSTANTIATE_TEST_SUITE_P(Pauses, SenderPause, ::testing::Values(200, 1000, 2000, 7000));

// #593 at 40 ms: the sender goes on capturing, nothing arrives for 1.2 s, then the backlog comes in a burst and the
// stream goes on. The backlog is stale — the head passed its time while the stream was out: dropped as late,
// concealed, never waited for. The target stays low, no delay is added past it (nothing to shed later), and no speech
// is cut.
TEST(AudioPlayoutCaptureTime, AnOutageBacklogIsLateAndAddsNoDelay) {
  Listener ear;
  std::vector<Sent> wire = Steady(1, 500, [](uint32_t seq) { return seq * kPacketMs; });
  const int64_t back_at = wire[249].recv_ms + 1200;
  for (Sent& s : wire) {
    if (s.seq > 250 && s.recv_ms < back_at) {
      s.recv_ms = back_at + (s.seq - 251) % 8; // held in the network, then all at once
    }
  }
  ear.Run(wire, wire.back().recv_ms + 500);

  const int64_t delay_before = Delay(ear, wire, 250);
  EXPECT_LE(ear.buf.TargetFrames(), 3u) << "an outage is not jitter";
  EXPECT_GE(ear.buf.drops_late(), 1200u / kPacketMs - 3) << "the stale backlog";
  EXPECT_EQ(ear.buf.speech_drops(), 0u);
  EXPECT_EQ(ear.buf.silence_drops(), 0u) << "no surplus to shed: no delay was added";
  const uint32_t first_after = 251 + static_cast<uint32_t>(ear.buf.drops_late());
  ASSERT_TRUE(ear.PlayedAt(first_after));
  EXPECT_LE(Delay(ear, wire, first_after), delay_before + kPacketMs);
  EXPECT_EQ(ear.MissingBetween(first_after, 500), 0u);
  EXPECT_LE(Delay(ear, wire, 450), delay_before + kSlotMs);
}

// The two ends' clocks differ by tens of ppm. Over 10 minutes at 100 ppm, either way: a sender faster than the
// listener queues more, shed in its silences; a slower one runs the buffer dry, and the underrun's wait moves the
// schedule later. Never past the cap, nothing late, no speech cut, nothing stuck.
class ClockDrift : public ::testing::TestWithParam<double> {};

TEST_P(ClockDrift, StaysWithinTheCapAndNeverSticks) {
  const double ppm = GetParam();
  constexpr uint32_t kPackets = 15000; // 10 minutes
  Listener ear;
  std::vector<Sent> wire;
  for (uint32_t seq = 1; seq <= kPackets; ++seq) {
    const int64_t at = int64_t{seq} * kPacketMs; // the sender's clock
    const int64_t sent_at = static_cast<int64_t>(static_cast<double>(at + kPacketMs) / (1.0 + ppm * 1e-6));
    const uint8_t fill = seq % 50 < 10 ? kSilence : kSpeech; // 0.4 s of silence every 2 s
    wire.push_back({.seq = seq, .capture_ms = at, .recv_ms = sent_at + kNetMs, .fill = fill});
  }
  ear.Run(wire, wire.back().recv_ms + 500);

  EXPECT_LE(ear.max_depth_ms, AudioJitterBuffer::kMaxDelayMs);
  EXPECT_EQ(ear.buf.drops_late(), 0u);
  EXPECT_EQ(ear.buf.speech_drops(), 0u);
  // Silence skipped to catch up is not played; everything else is.
  size_t speech_missing = 0;
  for (uint32_t seq = 1; seq <= kPackets; ++seq) {
    speech_missing += wire[seq - 1].fill == kSpeech && !ear.PlayedAt(seq) ? 1 : 0;
  }
  EXPECT_EQ(speech_missing, 0u);
  EXPECT_LE(Delay(ear, wire, kPackets - 5), AudioJitterBuffer::kMaxDelayMs);
  EXPECT_LE(ear.underrun_slots, 10u) << "the buffer does not run dry again and again";
}

INSTANTIATE_TEST_SUITE_P(Ppm, ClockDrift, ::testing::Values(100.0, -100.0));

// The capture time is a u32 of ms: it wraps every ~49.7 days of a sender's session, and playout runs straight through.
TEST(AudioPlayoutCaptureTime, TheCaptureClockWraps) {
  Listener ear;
  constexpr int64_t kNearWrap = (int64_t{1} << 32) - 3000;
  std::vector<Sent> wire = Steady(1, 250, [](uint32_t seq) { return kNearWrap + seq * kPacketMs; });
  for (Sent& s : wire) {
    s.recv_ms -= kNearWrap; // the receiver's own clock
  }
  ear.Run(wire, wire.back().recv_ms + 500);
  EXPECT_EQ(ear.buf.drops_late(), 0u);
  EXPECT_EQ(ear.concealed.size(), 0u);
  EXPECT_EQ(ear.MissingBetween(1, 250), 0u);
  EXPECT_EQ(ear.underrun_slots, 0u);
}

// A restarted sender is a new instance: its seq and its clock start over. The buffer starts over on it at once.
TEST(AudioPlayoutCaptureTime, ASenderRestartReprimes) {
  Listener ear;
  std::vector<Sent> wire = Steady(1, 250, [](uint32_t seq) { return 600'000 + seq * kPacketMs; });
  for (Sent& s : wire) {
    s.recv_ms -= 600'000;
  }
  const int64_t restart_at = wire.back().recv_ms + 300;
  std::vector<Sent> restarted = Steady(1, 250, [](uint32_t seq) { return seq * kPacketMs; });
  for (Sent& s : restarted) {
    s.recv_ms += restart_at;
    s.instance = 2;
  }
  wire.insert(wire.end(), restarted.begin(), restarted.end());
  ear.Run(wire, wire.back().recv_ms + 500);

  ASSERT_TRUE(ear.PlayedAt(1));
  EXPECT_LE(ear.played.back().seq, 250u);
  EXPECT_EQ(ear.buf.drops_late(), 0u);
  // Primed again at the minimum target: the restarted stream's first packet plays two packets after it came.
  int64_t first_of_restart = 0;
  for (const Played& p : ear.played) {
    if (p.at_ms > restart_at) {
      EXPECT_EQ(p.seq, 1u);
      first_of_restart = p.at_ms;
      break;
    }
  }
  EXPECT_LE(first_of_restart - restarted.front().recv_ms, 2 * kPacketMs + kSlotMs);
}

// A lost packet whose next one is queued is recovered from that one's FEC: a Gap slot for it, in its time.
TEST(AudioPlayoutCaptureTime, ALossIsRecoveredFromTheNextPacket) {
  Listener ear;
  std::vector<Sent> wire = Steady(1, 250, [](uint32_t seq) { return seq * kPacketMs; });
  wire.erase(wire.begin() + 99); // seq 100 never arrives
  ear.Run(wire, wire.back().recv_ms + 500);
  ASSERT_EQ(ear.concealed.size(), 1u);
  EXPECT_EQ(ear.concealed[0].seq, 100u);
  EXPECT_TRUE(ear.concealed[0].fec_usable);
  EXPECT_EQ(ear.MissingBetween(101, 250), 0u);
  EXPECT_EQ(*ear.PlayedAt(101) - *ear.PlayedAt(99), 2 * kPacketMs) << "the loss kept its time";
}

// Reordered within the target, and delivered twice: each packet plays once, in order, none late or concealed.
TEST(AudioPlayoutCaptureTime, ReorderingWithinTheTargetAndDuplicates) {
  Listener ear;
  std::vector<Sent> wire = Steady(1, 250, [](uint32_t seq) { return seq * kPacketMs; });
  for (size_t i = 20; i + 1 < wire.size(); i += 10) {
    std::swap(wire[i].recv_ms, wire[i + 1].recv_ms); // the later one first, 40 ms apart
  }
  const std::vector<Sent> copies(wire.begin() + 50, wire.begin() + 60);
  for (Sent copy : copies) {
    copy.recv_ms += 5;
    wire.push_back(copy);
  }
  ear.Run(wire, 250 * kPacketMs + 600);
  EXPECT_EQ(ear.MissingBetween(1, 250), 0u);
  EXPECT_EQ(ear.played.size(), 250u) << "a duplicate plays once";
  EXPECT_TRUE(std::is_sorted(ear.played.begin(), ear.played.end(),
                             [](const Played& a, const Played& b) { return a.seq < b.seq; }));
  EXPECT_EQ(ear.buf.drops_late(), 0u);
  EXPECT_EQ(ear.concealed.size(), 0u);
  EXPECT_EQ(ear.buf.TargetFrames(), 3u) << "a reorder within the target raises it at most a packet";
}

// A stream that stays behind the head — here a sender whose capture time stood still through a 1 s pause, as a seq
// clock would — is re-timed, not a backlog: after a second of late packets the buffer starts over on it. No stream is
// late forever.
TEST(AudioPlayoutCaptureTime, AStreamBehindTheHeadIsRetimedNotLateForever) {
  Listener ear;
  std::vector<Sent> wire = Steady(1, 500, [](uint32_t seq) { return seq * kPacketMs; });
  for (Sent& s : wire) {
    s.recv_ms += s.seq > 250 ? 1000 : 0; // a pause the capture time does not show
  }
  ear.Run(wire, wire.back().recv_ms + 500);
  EXPECT_LE(ear.buf.drops_late(), static_cast<uint64_t>(AudioJitterBuffer::kStaleRunMs / kPacketMs + 2));
  EXPECT_EQ(ear.MissingBetween(251 + AudioJitterBuffer::kStaleRunMs / kPacketMs + 2, 500), 0u);
}

} // namespace
} // namespace pbr
