#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace pbr {

/** Shortest packet the jitter buffer counts in (ms): the frame counts below are its, and longer packets divide them. */
inline constexpr int kAudioMinPacketMs = 20;
/** Longest Opus packet (120 ms). */
inline constexpr int kAudioMaxPacketMs = 120;

/** `ms` in packets of `packet_ms`, rounded up, at least 1. */
inline size_t AudioPacketsFor(int ms, int packet_ms) {
  return static_cast<size_t>(std::max(1, (ms + packet_ms - 1) / packet_ms));
}

/**
 * Arrival-jitter estimator for one publisher (adaptive jitter target, DEV/PLAN-adaptive-jitter.md §1).
 * Lateness = relative delay (recv_ms − seq·packet_ms) above its 5 s sliding minimum, bucketed per packet in a
 * forgetting histogram (~10 s memory). Target = 99th-percentile bucket + 1 packet, clamped to 60..300 ms
 * (3..15 packets of 20 ms, 2..7 of 40 ms). Rises as soon as late packets arrive; decays as old samples fade. Its top
 * stays below the buffer's 400 ms cap (V065's budget), so above the target there is room to catch up in silences.
 *
 * An outage is not jitter (#593: the stream stopped for 1.2 s, then its backlog came in a burst; the target pinned at
 * its top for the rest of the call). A packet later than the target's top could ever hold starts an outage, and the
 * backlog — every packet still later than the target was before it — is left out: no target could have played it in
 * time, so it is concealed, never waited for.
 */
class AudioArrivalJitter {
public:
  static constexpr int kMinTargetMs = 60;
  static constexpr int kMaxTargetMs = 300;
  /** At 20 ms packets. */
  static constexpr size_t kMinTargetFrames = kMinTargetMs / kAudioMinPacketMs;
  static constexpr size_t kMaxTargetFrames = kMaxTargetMs / kAudioMinPacketMs;
  static constexpr size_t kBaselinePackets = 250;
  static constexpr double kForget = 0.998;
  static constexpr double kQuantile = 0.99;

  /** The publisher's packet length (ms, kAudioMinPacketMs..kAudioMaxPacketMs); a change starts over. */
  void SetPacketMs(int packet_ms) {
    packet_ms = std::clamp(packet_ms, kAudioMinPacketMs, kAudioMaxPacketMs);
    if (packet_ms != packet_ms_) {
      Reset();
      packet_ms_ = packet_ms;
    }
  }
  int PacketMs() const { return packet_ms_; }
  size_t MinTargetFrames() const { return AudioPacketsFor(kMinTargetMs, packet_ms_); }
  size_t MaxTargetFrames() const { return static_cast<size_t>(kMaxTargetMs / packet_ms_); }

  void OnArrival(uint32_t seq, int64_t recv_ms) {
    const int64_t d = recv_ms - static_cast<int64_t>(seq) * packet_ms_;
    const uint64_t idx = arrivals_++;
    while (!mins_.empty() && mins_.back().second >= d) {
      mins_.pop_back();
    }
    mins_.emplace_back(idx, d);
    while (mins_.front().first + kBaselinePackets <= idx) {
      mins_.pop_front();
    }
    const int64_t late_ms = d - mins_.front().second;
    const int64_t held_ms = static_cast<int64_t>(TargetFrames() + 1) * packet_ms_;
    if (late_ms > kMaxTargetMs) {
      outage_ = true;
    }
    if (outage_ && late_ms <= held_ms) {
      outage_ = false; // on time again: what is older than this packet and late is still the backlog
      backlog_below_seq_ = seq;
    }
    if (late_ms > held_ms && (outage_ || seq < backlog_below_seq_)) {
      ++outage_arrivals_;
      return; // the outage's backlog
    }
    const size_t bucket = std::min<size_t>(static_cast<size_t>(late_ms / packet_ms_), MaxTargetFrames());
    for (double& p : hist_) {
      p *= kForget;
    }
    hist_[bucket] += 1.0 - kForget;
    mass_ = mass_ * kForget + (1.0 - kForget);
  }

  size_t TargetFrames() const {
    if (mass_ <= 0.0) {
      return MinTargetFrames();
    }
    double acc = 0.0;
    for (size_t k = 0; k < hist_.size(); ++k) {
      acc += hist_[k];
      if (acc >= kQuantile * mass_) {
        return std::clamp(k + 1, MinTargetFrames(), MaxTargetFrames());
      }
    }
    return MaxTargetFrames();
  }

  /** Packets left out of the estimate as an outage's backlog. */
  uint64_t OutageArrivals() const { return outage_arrivals_; }

  void Reset() {
    hist_.fill(0.0);
    mass_ = 0.0;
    arrivals_ = 0;
    mins_.clear();
    outage_ = false;
    backlog_below_seq_ = 0;
  }

private:
  std::array<double, kMaxTargetFrames + 1> hist_{};
  int packet_ms_ = kAudioMinPacketMs;
  double mass_ = 0.0;
  uint64_t arrivals_ = 0;
  bool outage_ = false;
  /** The first on-time packet after an outage: late packets older than it are that outage's backlog too. */
  uint32_t backlog_below_seq_ = 0;
  uint64_t outage_arrivals_ = 0;
  std::deque<std::pair<uint64_t, int64_t>> mins_; // (arrival index, d) — monotonic sliding minimum
};

/** One received Opus packet (channel 0) for the jitter buffer. */
struct AudioPacket {
  uint32_t seq = 0;
  /** Monotonic (steady_clock) receive time in ms — not wall clock, so an NTP step can't look
   *  like packet lateness to the arrival-jitter estimator (M6). */
  int64_t recv_ms = 0;
  /** Audio the packet holds (ms, from its Opus TOC); 0 = kAudioMinPacketMs. */
  int duration_ms = 0;
  std::vector<uint8_t> payload;
};

/** Result of one playout pop. */
struct AudioPlayoutPop {
  enum class Kind { Empty, Packet, Gap };
  Kind kind = Kind::Empty;
  /** Packet: this packet's seq. Gap: the missing seq. */
  uint32_t seq = 0;
  /** Packet: its bytes. Gap: the NEXT queued packet's bytes (Opus in-band FEC source). */
  std::vector<uint8_t> payload;
  /** Gap only: `payload` is the packet right after the missing seq, so its LBRR covers it. */
  bool fec_usable = false;
};

/**
 * Per-publisher packet jitter buffer (receiver only; hop stays blind).
 * Target delay adapts to arrival jitter (AudioArrivalJitter: 60–300 ms); playout holds at most kMaxDelayMs (400 ms,
 * V065's budget). It counts in the publisher's packets, whose length the first packet tells (20 ms → 3..15 / cap 20,
 * 40 ms → 2..7 / cap 10); the `k*Frames` constants are the 20 ms counts. Packets are decoded at pop time so a missing
 * seq can be recovered from the next packet's FEC data (spec §1/§2).
 *
 * Catch-up is never heard (V065): above the target the engine skips decoded silence (NoteSilenceDrop); over the cap
 * it skips silence first, as much as the excess, and only then cuts speech to the cap (CutToCap, oldest first). Push
 * itself cuts only past kHardMaxDelayMs: a burst while nothing is popped (a device pause), bounded memory.
 */
class AudioJitterBuffer {
public:
  static constexpr int kFrameMs = kAudioMinPacketMs;
  static constexpr int kTargetDelayMs = AudioArrivalJitter::kMinTargetMs;
  /** The most audio playout holds (V065: ~300 ms mouth to ear, 400 ms at most). */
  static constexpr int kMaxDelayMs = 400;
  static_assert(AudioArrivalJitter::kMaxTargetMs < kMaxDelayMs, "the cap leaves room above the target");
  /** What a burst may queue between pops (nothing popped: a device pause) before Push cuts it. */
  static constexpr int kHardMaxDelayMs = 2 * kMaxDelayMs;
  static constexpr size_t kTargetFrames = AudioArrivalJitter::kMinTargetFrames; // kept: minimum/steady target
  static constexpr size_t kMaxFrames = kMaxDelayMs / kFrameMs;
  static constexpr size_t kHardMaxFrames = kHardMaxDelayMs / kFrameMs;
  /** The pre-adaptive 200 ms cap: congestion signal for bitrate adaptation. */
  static constexpr int kPressureFullMs = 200;
  /** A seq this far behind the expected one is a sender restart (seq reset), not a late packet. */
  static constexpr uint32_t kResyncJump = 50;
  /** This many late packets in a row with a restarted stream's low seqs (< kResyncJump) is a
   *  sender restart even when the backward step is small (early in a session next_seq_ < kResyncJump,
   *  so the jump test alone would drop ~1 s of the new stream). Late stragglers with higher seqs —
   *  e.g. a reorder burst from path migration mid-session — are never a restart. */
  static constexpr uint32_t kRestartLateRun = 3;

  void Push(AudioPacket packet) {
    if (packet.payload.empty()) {
      return;
    }
    const int packet_ms =
        std::clamp(packet.duration_ms > 0 ? packet.duration_ms : kFrameMs, kAudioMinPacketMs, kAudioMaxPacketMs);
    if (packet_ms != PacketMs()) {
      arrival_.SetPacketMs(packet_ms);
      Reset(); // another packet length: a restarted sender (another build) — start over in its packets
    }
    if (primed_ && packet.seq < next_seq_) {
      // Behind by less than a restart's jump (beyond what the last outage concealed): late, not a restarted sender.
      if (packet.seq + kResyncJump + concealed_seqs_ >= next_seq_) {
        late_run_ = packet.seq < kResyncJump ? late_run_ + 1 : 0;
        if (late_run_ < kRestartLateRun) {
          arrival_.OnArrival(packet.seq, packet.recv_ms);
          ++drops_late_;
          return;
        }
      }
      Reset(); // sender restarted its seq (re-StartDuplex / BeginSession): re-prime on the new stream
    }
    late_run_ = 0;
    auto it = queue_.begin();
    while (it != queue_.end() && it->seq < packet.seq) {
      ++it;
    }
    if (it != queue_.end() && it->seq == packet.seq) {
      return; // duplicate — not lateness, don't feed the estimator (M4)
    }
    arrival_.OnArrival(packet.seq, packet.recv_ms);
    queue_.insert(it, std::move(packet));
    if (queue_.size() > HardMaxFrames()) {
      CutTo(HardMaxFrames());
    }
  }

  size_t size() const { return queue_.size(); }
  /** The publisher's packet length (ms): what one queued packet, a Gap, and a target frame stand for. */
  int PacketMs() const { return arrival_.PacketMs(); }
  uint64_t drops_overflow() const { return drops_overflow_; }
  uint64_t drops_late() const { return drops_late_; }
  uint64_t underruns() const { return underruns_; }
  uint64_t gaps() const { return gaps_; }
  /** Adaptive target from the arrival-jitter estimator (AudioArrivalJitter), 60..400 ms in packets. */
  size_t TargetFrames() const { return arrival_.TargetFrames(); }
  /** True once the buffer holds more than one frame of slack above target. */
  bool OverTarget() const { return queue_.size() > TargetFrames() + 1; }
  /** The cap in the publisher's packets (kMaxDelayMs). */
  size_t CapFrames() const { return static_cast<size_t>(kMaxDelayMs / PacketMs()); }
  /** Packets queued past the cap (0 within it): what catch-up has to shed, silence first. */
  size_t ExcessOverCap() const { return queue_.size() > CapFrames() ? queue_.size() - CapFrames() : 0; }
  /** Over the cap with no silence left to skip: speech goes, oldest first (counted as speech drops). */
  void CutToCap() {
    if (queue_.size() > CapFrames()) {
      CutTo(CapFrames());
    }
  }
  /** Engine dropped a decoded silent frame to catch up (known-silence, not counted as speech). */
  void NoteSilenceDrop() { ++drops_silence_; }
  uint64_t silence_drops() const { return drops_silence_; }
  /** Packets cut at the cap or the hard bound: content unknown, so counted as speech. */
  uint64_t speech_drops() const { return drops_overflow_; }

  /**
   * How long an underrun waits for the next packet (ms) before its slots are concealed: up to the target's top, so
   * the backlog that ends an outage still fits under the cap and is shed in silences, not cut.
   */
  int WaitBudgetMs() const {
    return std::max(0, AudioArrivalJitter::kMaxTargetMs - static_cast<int>(TargetFrames()) * PacketMs());
  }

  /**
   * One packet's playout. Before the target depth was reached once → Empty (no underrun).
   * Then: queue empty → Empty + underrun (one 20 ms slot). An underrun waits for the next packet while the delay stays
   * within the target's top (WaitBudgetMs); past it the slots are concealed, never waited for (V065): the expected seq moves on
   * with time, and a packet that comes after its slot is a late drop. Next expected packet → Packet. A hole before the front
   * is skipped (front played as Packet) when the buffer already holds more than target frames or
   * the hole is wider than the target; otherwise each missing seq is a Gap slot (front stays
   * queued, its bytes returned; `fec_usable` only for the seq right before it, which its LBRR
   * covers).
   */
  AudioPlayoutPop PopForPlayout() {
    AudioPlayoutPop out;
    if (!primed_) {
      // Prime at the minimum target (60 ms), not the adaptive one: a single early outlier can
      // otherwise skew TargetFrames() (tiny sample count) and delay first audio for no reason
      // (M3). The adaptive target keeps governing hole-skip, OverTarget and trim below.
      if (queue_.size() < arrival_.MinTargetFrames()) {
        return out;
      }
      primed_ = true;
      next_seq_ = queue_.front().seq;
    }
    if (queue_.empty()) {
      ++underruns_;
      if (waited_ms_ == 0) {
        concealed_seqs_ = 0; // a new underrun
      }
      waited_ms_ += kAudioMinPacketMs;
      if (waited_ms_ > WaitBudgetMs()) {
        concealed_ms_ += kAudioMinPacketMs;
        if (concealed_ms_ >= PacketMs()) {
          concealed_ms_ -= PacketMs();
          ++next_seq_; // its slot has passed
          ++concealed_seqs_;
        }
      }
      return out;
    }
    waited_ms_ = 0;
    concealed_ms_ = 0;
    AudioPacket& front = queue_.front();
    if (front.seq > next_seq_ && (queue_.size() > TargetFrames() || front.seq - next_seq_ > TargetFrames())) {
      next_seq_ = front.seq; // enough audio buffered, or hole too wide to conceal usefully
    }
    if (front.seq == next_seq_) {
      out.kind = AudioPlayoutPop::Kind::Packet;
      out.seq = front.seq;
      out.payload = std::move(front.payload);
      queue_.pop_front();
      ++next_seq_;
      return out;
    }
    // Small hole in a shallow buffer: conceal one missing seq.
    out.kind = AudioPlayoutPop::Kind::Gap;
    out.seq = next_seq_;
    out.payload = front.payload;
    out.fec_usable = front.seq == next_seq_ + 1;
    ++gaps_;
    ++next_seq_;
    return out;
  }

  void Reset() {
    queue_.clear();
    primed_ = false;
    next_seq_ = 0;
    late_run_ = 0;
    waited_ms_ = 0;
    concealed_ms_ = 0;
    concealed_seqs_ = 0;
    arrival_.Reset();
  }

  /**
   * 0 = healthy, 1 = severe (underruns dominate). The "full" reference tracks the adaptive target
   * so a buffer sitting at its own target+1 (e.g. held there by silence catch-up) doesn't read as
   * congested; at the minimum target (60 ms) this is the original fixed 200 ms reference (I1).
   * `window_pops` counts 20 ms playout slots, as underruns do.
   */
  double Pressure(uint64_t window_pops) const {
    const size_t full = TargetFrames() + AudioPacketsFor(kPressureFullMs - kTargetDelayMs, PacketMs());
    if (window_pops == 0) {
      return queue_.size() >= full ? 1.0 : 0.0;
    }
    const double u = static_cast<double>(underruns_) / static_cast<double>(window_pops);
    const double fill = static_cast<double>(queue_.size()) / static_cast<double>(full);
    return std::min(1.0, std::max(u * 2.0, fill > 0.9 ? fill : 0.0));
  }

private:
  size_t HardMaxFrames() const { return static_cast<size_t>(kHardMaxDelayMs / PacketMs()); }

  /** Drop the oldest packets down to `frames`; seqs older than the new front can never play: no Gap for them. */
  void CutTo(const size_t frames) {
    while (queue_.size() > frames) {
      queue_.pop_front();
      ++drops_overflow_;
    }
    if (primed_ && !queue_.empty()) {
      next_seq_ = std::max(next_seq_, queue_.front().seq);
    }
  }

  AudioArrivalJitter arrival_;
  std::deque<AudioPacket> queue_;
  bool primed_ = false;
  uint32_t next_seq_ = 0;
  uint32_t late_run_ = 0;
  uint64_t drops_overflow_ = 0;
  uint64_t drops_late_ = 0;
  uint64_t drops_silence_ = 0;
  uint64_t underruns_ = 0;
  uint64_t gaps_ = 0;
  /** The current underrun: how long it has waited, and the concealed time not yet a whole packet. */
  int waited_ms_ = 0;
  int concealed_ms_ = 0;
  /** Seqs the last underrun concealed: their packets, still on the way, are late rather than a restart. */
  uint32_t concealed_seqs_ = 0;
};

/** Peak below 1 % of full scale (≈ −40 dBFS): safe to skip when catching up (adaptive jitter §2). */
constexpr int kCatchUpSilencePeak = 328; // 0.01 × 32768
inline bool IsCatchUpSilence(const std::vector<int16_t>& pcm, size_t samples) {
  const size_t n = std::min(samples, pcm.size());
  for (size_t i = 0; i < n; ++i) {
    if (pcm[i] >= kCatchUpSilencePeak || pcm[i] <= -kCatchUpSilencePeak) {
      return false;
    }
  }
  return true;
}

/** Saturating mix of mono s16 frames into `out` (size = samples). */
inline void MixPcmSat(std::vector<int16_t>& out, const std::vector<int16_t>& in) {
  const size_t n = std::min(out.size(), in.size());
  for (size_t i = 0; i < n; ++i) {
    const int sum = static_cast<int>(out[i]) + static_cast<int>(in[i]);
    out[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, sum)));
  }
}

/**
 * Playout gain with a soft knee: linear up to 70 % of full scale, then tanh-compressed so a
 * boosted peak approaches but never clips full scale (no hard-clip distortion).
 */
inline void ApplySoftGain(std::vector<int16_t>& pcm, float gain) {
  constexpr float kKnee = 0.7f;
  for (auto& s : pcm) {
    float y = static_cast<float>(s) / 32768.f * gain;
    const float a = std::fabs(y);
    if (a > kKnee) {
      y = std::copysign(kKnee + (1.f - kKnee) * std::tanh((a - kKnee) / (1.f - kKnee)), y);
    }
    s = static_cast<int16_t>(std::lrint(std::clamp(y * 32768.f, -32768.f, 32767.f)));
  }
}

} // namespace pbr
