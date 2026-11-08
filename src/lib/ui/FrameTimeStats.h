#pragma once

#include <cstddef>
#include <vector>

namespace pbr {

/** Summary of the frame intervals collected in one window. */
struct FrameTimeSummary {
  size_t frames = 0;
  double avg_fps = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
  /** Frames that took longer than 50 ms (three 60 Hz frames). */
  size_t over50 = 0;
};

/**
 * Collects frame-to-frame durations (milliseconds) for the developer engine test page.
 * Add() only appends to a pre-sized buffer, so it is safe to call every frame; Summarize() sorts a
 * copy and is meant to be called every second or so, not per frame.
 */
class FrameTimeStats {
public:
  /** Samples beyond this many in one window are dropped (10 s at 240 Hz fits twice over). */
  static constexpr size_t kCapacity = 8192;

  FrameTimeStats();

  void Add(double frame_ms);
  void Reset();
  size_t Count() const { return samples_.size(); }

  /** avg_fps = frames / total time; p99 = nearest-rank 99th percentile of the frame times. */
  FrameTimeSummary Summarize() const;

private:
  std::vector<float> samples_;
};

} // namespace pbr
