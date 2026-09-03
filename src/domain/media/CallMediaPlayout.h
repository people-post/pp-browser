#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace pbr {

/** One decoded PCM frame (mono s16) for jitter / mix (V032). */
struct PlayoutPcmFrame {
  uint32_t seq = 0;
  int64_t recv_ms = 0;
  std::vector<int16_t> pcm;
};

/**
 * Per-publisher audio jitter buffer (receiver only; hop stays blind).
 * Target delay 60 ms / max 200 ms at 20 ms frames → 3 / 10 frames.
 */
class AudioJitterBuffer {
public:
  static constexpr int kFrameMs = 20;
  static constexpr int kTargetDelayMs = 60;
  static constexpr int kMaxDelayMs = 200;
  static constexpr size_t kTargetFrames = static_cast<size_t>(kTargetDelayMs / kFrameMs);
  static constexpr size_t kMaxFrames = static_cast<size_t>(kMaxDelayMs / kFrameMs);

  void Push(PlayoutPcmFrame frame) {
    if (frame.pcm.empty()) {
      return;
    }
    // Ordered insert by seq (small queues).
    auto it = queue_.begin();
    while (it != queue_.end() && it->seq < frame.seq) {
      ++it;
    }
    if (it != queue_.end() && it->seq == frame.seq) {
      return; // duplicate
    }
    queue_.insert(it, std::move(frame));
    while (queue_.size() > kMaxFrames) {
      queue_.pop_front();
      ++drops_overflow_;
    }
  }

  size_t size() const { return queue_.size(); }
  uint64_t drops_overflow() const { return drops_overflow_; }
  uint64_t underruns() const { return underruns_; }
  uint64_t plc_frames() const { return plc_frames_; }

  /**
   * Pop one frame for playout. Returns nullopt + increments underrun when empty
   * (caller may synthesize PLC silence). Prefers waiting until target depth once.
   */
  std::optional<PlayoutPcmFrame> PopForPlayout(bool allow_underrun_plc) {
    if (!primed_) {
      if (queue_.size() < kTargetFrames) {
        return std::nullopt;
      }
      primed_ = true;
    }
    if (queue_.empty()) {
      ++underruns_;
      if (allow_underrun_plc) {
        ++plc_frames_;
      }
      return std::nullopt;
    }
    PlayoutPcmFrame out = std::move(queue_.front());
    queue_.pop_front();
    return out;
  }

  void Reset() {
    queue_.clear();
    primed_ = false;
  }

  /** 0 = healthy, 1 = severe (underruns dominate). */
  double Pressure(uint64_t window_pops) const {
    if (window_pops == 0) {
      return queue_.size() >= kMaxFrames ? 1.0 : 0.0;
    }
    const double u = static_cast<double>(underruns_) / static_cast<double>(window_pops);
    const double fill = static_cast<double>(queue_.size()) / static_cast<double>(kMaxFrames);
    return std::min(1.0, std::max(u * 2.0, fill > 0.9 ? fill : 0.0));
  }

private:
  std::deque<PlayoutPcmFrame> queue_;
  bool primed_ = false;
  uint64_t drops_overflow_ = 0;
  uint64_t underruns_ = 0;
  uint64_t plc_frames_ = 0;
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
