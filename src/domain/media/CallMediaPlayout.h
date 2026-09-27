#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pbr {

/** One received Opus packet (channel 0) for the jitter buffer. */
struct AudioPacket {
  uint32_t seq = 0;
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
 * Target delay 60 ms / max 200 ms at 20 ms frames → 3 / 10 packets. Packets are decoded at
 * pop time so a missing seq can be recovered from the next packet's FEC data (spec §1/§2).
 */
class AudioJitterBuffer {
public:
  static constexpr int kFrameMs = 20;
  static constexpr int kTargetDelayMs = 60;
  static constexpr int kMaxDelayMs = 200;
  static constexpr size_t kTargetFrames = static_cast<size_t>(kTargetDelayMs / kFrameMs);
  static constexpr size_t kMaxFrames = static_cast<size_t>(kMaxDelayMs / kFrameMs);
  /** A seq this far behind the expected one is a sender restart (seq reset), not a late packet. */
  static constexpr uint32_t kResyncJump = 50;
  /** Surplus depth held for a whole window (500 ms) is trimmed back to the target. */
  static constexpr uint32_t kDrainWindowPops = 25;

  void Push(AudioPacket packet) {
    if (packet.payload.empty()) {
      return;
    }
    if (primed_ && packet.seq < next_seq_) {
      if (packet.seq + kResyncJump >= next_seq_) {
        ++drops_late_;
        return;
      }
      Reset(); // sender restarted its seq (re-StartSfu / BeginSession): re-prime on the new stream
    }
    auto it = queue_.begin();
    while (it != queue_.end() && it->seq < packet.seq) {
      ++it;
    }
    if (it != queue_.end() && it->seq == packet.seq) {
      return; // duplicate
    }
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
        (queue_.size() > kTargetFrames || front.seq - next_seq_ > kTargetFrames)) {
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
    window_pops_ = 0;
    window_min_depth_ = kMaxFrames;
  }

  /** 0 = healthy, 1 = severe (underruns dominate). Unchanged from the PCM buffer. */
  double Pressure(uint64_t window_pops) const {
    if (window_pops == 0) {
      return queue_.size() >= kMaxFrames ? 1.0 : 0.0;
    }
    const double u = static_cast<double>(underruns_) / static_cast<double>(window_pops);
    const double fill = static_cast<double>(queue_.size()) / static_cast<double>(kMaxFrames);
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
   * device pause can fill the queue; if the depth never fell below target + 1 over a whole
   * window, that surplus absorbed no jitter — drop it (oldest first) back to the target.
   */
  void DrainSurplus() {
    window_min_depth_ = std::min(window_min_depth_, queue_.size());
    if (++window_pops_ < kDrainWindowPops) {
      return;
    }
    if (window_min_depth_ > kTargetFrames + 1) {
      for (size_t n = window_min_depth_ - kTargetFrames; n > 0; --n) {
        queue_.pop_front();
        ++drops_overflow_;
      }
      SkipDroppedSeqs();
    }
    window_pops_ = 0;
    window_min_depth_ = kMaxFrames;
  }

  std::deque<AudioPacket> queue_;
  bool primed_ = false;
  uint32_t next_seq_ = 0;
  uint64_t drops_overflow_ = 0;
  uint64_t drops_late_ = 0;
  uint64_t underruns_ = 0;
  uint64_t gaps_ = 0;
  uint32_t window_pops_ = 0;
  size_t window_min_depth_ = kMaxFrames;
};

/** Saturating mix of mono s16 frames into `out` (size = samples). */
inline void MixPcmSat(std::vector<int16_t>& out, const std::vector<int16_t>& in) {
  const size_t n = std::min(out.size(), in.size());
  for (size_t i = 0; i < n; ++i) {
    const int sum = static_cast<int>(out[i]) + static_cast<int>(in[i]);
    out[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, sum)));
  }
}

} // namespace pbr
