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

/**
 * Arrival-jitter estimator for one publisher (adaptive jitter target, DEV/PLAN-adaptive-jitter.md §1).
 * Lateness = relative delay (recv_ms − seq·20) above its 5 s sliding minimum, bucketed per 20 ms in a
 * forgetting histogram (~10 s memory). Target = 99th-percentile bucket + 1 frame, clamped 3..20.
 * Rises as soon as late packets arrive; decays as old samples fade.
 */
class AudioArrivalJitter {
public:
  static constexpr int kFrameMs = 20;
  static constexpr size_t kMinTargetFrames = 3;
  static constexpr size_t kMaxTargetFrames = 20;
  static constexpr size_t kBaselinePackets = 250;
  static constexpr double kForget = 0.998;
  static constexpr double kQuantile = 0.99;

  void OnArrival(uint32_t seq, int64_t recv_ms) {
    const int64_t d = recv_ms - static_cast<int64_t>(seq) * kFrameMs;
    const uint64_t idx = arrivals_++;
    while (!mins_.empty() && mins_.back().second >= d) {
      mins_.pop_back();
    }
    mins_.emplace_back(idx, d);
    while (mins_.front().first + kBaselinePackets <= idx) {
      mins_.pop_front();
    }
    const int64_t late_ms = d - mins_.front().second;
    const size_t bucket = std::min<size_t>(static_cast<size_t>(late_ms / kFrameMs), kMaxTargetFrames);
    for (double& p : hist_) {
      p *= kForget;
    }
    hist_[bucket] += 1.0 - kForget;
    mass_ = mass_ * kForget + (1.0 - kForget);
  }

  size_t TargetFrames() const {
    if (mass_ <= 0.0) {
      return kMinTargetFrames;
    }
    double acc = 0.0;
    for (size_t k = 0; k < hist_.size(); ++k) {
      acc += hist_[k];
      if (acc >= kQuantile * mass_) {
        return std::clamp(k + 1, kMinTargetFrames, kMaxTargetFrames);
      }
    }
    return kMaxTargetFrames;
  }

  void Reset() {
    hist_.fill(0.0);
    mass_ = 0.0;
    arrivals_ = 0;
    mins_.clear();
  }

private:
  std::array<double, kMaxTargetFrames + 1> hist_{};
  double mass_ = 0.0;
  uint64_t arrivals_ = 0;
  std::deque<std::pair<uint64_t, int64_t>> mins_;  // (arrival index, d) — monotonic sliding minimum
};

/** One received Opus packet (channel 0) for the jitter buffer. */
struct AudioPacket {
  uint32_t seq = 0;
  /** Monotonic (steady_clock) receive time in ms — not wall clock, so an NTP step can't look
   *  like packet lateness to the arrival-jitter estimator (M6). */
  int64_t recv_ms = 0;
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
 * Target delay adapts to arrival jitter (AudioArrivalJitter: 60–400 ms) / max 800 ms at 20 ms
 * frames → 3..20 / 40 packets. Packets are decoded at pop time so a missing seq can be recovered
 * from the next packet's FEC data (spec §1/§2).
 */
class AudioJitterBuffer {
public:
  static constexpr int kFrameMs = 20;
  static constexpr int kTargetDelayMs = 60;
  static constexpr int kMaxDelayMs = 800;
  static constexpr size_t kTargetFrames = AudioArrivalJitter::kMinTargetFrames; // kept: minimum/steady target
  static constexpr size_t kMaxFrames = 40;
  /** Surplus above target this size is treated as speech content (unknown), not pure jitter slack. */
  static constexpr size_t kSpeechTrimSurplus = 5;
  static constexpr size_t kPressureFullFrames = 10; // the pre-adaptive 200 ms cap: congestion signal for bitrate adaptation
  /** A seq this far behind the expected one is a sender restart (seq reset), not a late packet. */
  static constexpr uint32_t kResyncJump = 50;
  /** Surplus depth held for a whole window (nominally 500 ms; a pop is a pop, so silence
   *  catch-up's extra pops per slot advance the window too — it can complete sooner) is trimmed
   *  back to the target. */
  static constexpr uint32_t kDrainWindowPops = 25;
  /** This many late packets in a row with a restarted stream's low seqs (< kResyncJump) is a
   *  sender restart even when the backward step is small (early in a call next_seq_ < kResyncJump,
   *  so the jump test alone would drop ~1 s of the new stream). Late stragglers with higher seqs —
   *  e.g. a reorder burst from path migration mid-call — are never a restart. */
  static constexpr uint32_t kRestartLateRun = 3;

  void Push(AudioPacket packet) {
    if (packet.payload.empty()) {
      return;
    }
    if (primed_ && packet.seq < next_seq_) {
      if (packet.seq + kResyncJump >= next_seq_) {
        late_run_ = packet.seq < kResyncJump ? late_run_ + 1 : 0;
        if (late_run_ < kRestartLateRun) {
          arrival_.OnArrival(packet.seq, packet.recv_ms);
          ++drops_late_;
          return;
        }
      }
      Reset(); // sender restarted its seq (re-StartSfu / BeginSession): re-prime on the new stream
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
    if (queue_.size() > kMaxFrames) {
      while (queue_.size() > kMaxFrames) {
        queue_.pop_front();
        ++drops_overflow_;
      }
      SkipDroppedSeqs();
    }
  }

  size_t size() const { return queue_.size(); }
  uint64_t drops_overflow() const { return drops_overflow_; }
  uint64_t drops_late() const { return drops_late_; }
  uint64_t underruns() const { return underruns_; }
  uint64_t gaps() const { return gaps_; }
  /** Adaptive target from the arrival-jitter estimator (AudioArrivalJitter), 3..20 frames. */
  size_t TargetFrames() const { return arrival_.TargetFrames(); }
  /** True once the buffer holds more than one frame of slack above target. */
  bool OverTarget() const { return queue_.size() > TargetFrames() + 1; }
  /** Engine dropped a decoded silent frame to catch up (known-silence, not counted as speech). */
  void NoteSilenceDrop() { ++drops_silence_; }
  uint64_t silence_drops() const { return drops_silence_; }
  /** Overflow + trim drops: content unknown, so counted as speech. */
  uint64_t speech_drops() const { return drops_overflow_; }

  /**
   * One 20 ms playout slot. Before the target depth was reached once → Empty (no underrun).
   * Then: queue empty → Empty + underrun; next expected packet → Packet. A hole before the front
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
      if (queue_.size() < kTargetFrames) {
        return out;
      }
      primed_ = true;
      next_seq_ = queue_.front().seq;
    }
    DrainSurplus();
    if (queue_.empty()) {
      ++underruns_;
      return out;
    }
    AudioPacket& front = queue_.front();
    if (front.seq > next_seq_ &&
        (queue_.size() > TargetFrames() || front.seq - next_seq_ > TargetFrames())) {
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
    window_pops_ = 0;
    window_min_depth_ = kMaxFrames;
    arrival_.Reset();
  }

  /**
   * 0 = healthy, 1 = severe (underruns dominate). The "full" reference tracks the adaptive target
   * so a buffer sitting at its own target+1 (e.g. held there by silence catch-up) doesn't read as
   * congested; at the minimum target (3) this is the original fixed 10-frame / 200 ms reference
   * (I1).
   */
  double Pressure(uint64_t window_pops) const {
    const size_t full = TargetFrames() + (kPressureFullFrames - kTargetFrames);
    if (window_pops == 0) {
      return queue_.size() >= full ? 1.0 : 0.0;
    }
    const double u = static_cast<double>(underruns_) / static_cast<double>(window_pops);
    const double fill = static_cast<double>(queue_.size()) / static_cast<double>(full);
    return std::min(1.0, std::max(u * 2.0, fill > 0.9 ? fill : 0.0));
  }

private:
  /** After we dropped from the front, seqs older than it can never play: don't Gap for them. */
  void SkipDroppedSeqs() {
    if (primed_ && !queue_.empty()) {
      next_seq_ = std::max(next_seq_, queue_.front().seq);
    }
  }

  /**
   * Underruns add a frame of latency each (time passes, nothing is consumed) and a burst or a
   * device pause can fill the queue; if the depth never fell below target + kSpeechTrimSurplus
   * over a whole window, that surplus absorbed no jitter — drop it (oldest first) back to the
   * target.
   */
  void DrainSurplus() {
    window_min_depth_ = std::min(window_min_depth_, queue_.size());
    if (++window_pops_ < kDrainWindowPops) {
      return;
    }
    if (window_min_depth_ > TargetFrames() + kSpeechTrimSurplus) {
      for (size_t n = window_min_depth_ - (TargetFrames() + 1); n > 0; --n) {
        queue_.pop_front();
        ++drops_overflow_;
      }
      SkipDroppedSeqs();
    }
    window_pops_ = 0;
    window_min_depth_ = kMaxFrames;
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
  uint32_t window_pops_ = 0;
  size_t window_min_depth_ = kMaxFrames;
};

/** Peak below 1 % of full scale (≈ −40 dBFS): safe to skip when catching up (adaptive jitter §2). */
constexpr int kCatchUpSilencePeak = 328;  // 0.01 × 32768
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
