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
  /** A jump this large ahead of the expected seq is a sender restart, not loss. */
  static constexpr uint32_t kResyncJump = 50;

  void Push(AudioPacket packet) {
    if (packet.payload.empty()) {
      return;
    }
    if (primed_ && packet.seq < next_seq_) {
      ++drops_late_;
      return;
    }
    auto it = queue_.begin();
    while (it != queue_.end() && it->seq < packet.seq) {
      ++it;
    }
    if (it != queue_.end() && it->seq == packet.seq) {
      return; // duplicate
    }
    queue_.insert(it, std::move(packet));
    while (queue_.size() > kMaxFrames) {
      queue_.pop_front();
      ++drops_overflow_;
    }
  }

  size_t size() const { return queue_.size(); }
  uint64_t drops_overflow() const { return drops_overflow_; }
  uint64_t drops_late() const { return drops_late_; }
  uint64_t underruns() const { return underruns_; }
  uint64_t gaps() const { return gaps_; }

  /**
   * One 20 ms playout slot. Before the target depth was reached once → Empty (no underrun).
   * Then: next expected packet → Packet; queue non-empty but front is ahead → Gap (with the
   * front's bytes for FEC, front stays queued); queue empty → Empty + underrun.
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
    if (queue_.empty()) {
      ++underruns_;
      return out;
    }
    AudioPacket& front = queue_.front();
    if (front.seq > next_seq_ + kResyncJump) {
      next_seq_ = front.seq; // sender restart (SoftMigrate / re-StartSfu)
    }
    if (front.seq == next_seq_) {
      out.kind = AudioPlayoutPop::Kind::Packet;
      out.seq = front.seq;
      out.payload = std::move(front.payload);
      queue_.pop_front();
      ++next_seq_;
      return out;
    }
    // front.seq > next_seq_: one missing slot.
    out.kind = AudioPlayoutPop::Kind::Gap;
    out.seq = next_seq_;
    out.payload = front.payload;
    ++gaps_;
    ++next_seq_;
    return out;
  }

  void Reset() {
    queue_.clear();
    primed_ = false;
    next_seq_ = 0;
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
  std::deque<AudioPacket> queue_;
  bool primed_ = false;
  uint32_t next_seq_ = 0;
  uint64_t drops_overflow_ = 0;
  uint64_t drops_late_ = 0;
  uint64_t underruns_ = 0;
  uint64_t gaps_ = 0;
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
