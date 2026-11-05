#include "lib/ui/FrameTimeStats.h"

#include <algorithm>

namespace pbr {

FrameTimeStats::FrameTimeStats() {
  samples_.reserve(kCapacity);
}

void FrameTimeStats::Add(double frame_ms) {
  if (frame_ms <= 0.0 || samples_.size() >= kCapacity) {
    return;
  }
  samples_.push_back(static_cast<float>(frame_ms));
}

void FrameTimeStats::Reset() {
  samples_.clear();
}

FrameTimeSummary FrameTimeStats::Summarize() const {
  FrameTimeSummary out;
  out.frames = samples_.size();
  if (samples_.empty()) {
    return out;
  }
  std::vector<float> sorted = samples_;
  std::sort(sorted.begin(), sorted.end());
  double total_ms = 0.0;
  for (const float ms : sorted) {
    total_ms += ms;
    if (ms > 50.0f) {
      ++out.over50;
    }
  }
  out.avg_fps = total_ms > 0.0 ? static_cast<double>(sorted.size()) * 1000.0 / total_ms : 0.0;
  const size_t rank = (sorted.size() * 99 + 99) / 100 /* ceil(0.99 n) */;
  out.p99_ms = sorted[std::min(sorted.size(), std::max<size_t>(rank, 1)) - 1];
  out.max_ms = sorted.back();
  return out;
}

} // namespace pbr
