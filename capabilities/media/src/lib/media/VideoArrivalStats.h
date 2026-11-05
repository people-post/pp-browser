#pragma once

#include "common/media_engine/MediaEngineHealth.h"

#include <array>
#include <cstdint>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Arrival timing of one remote video stream, measured where its packets reach the engine (before decoding):
 * the transport's share of how smooth video plays. Even arrivals (gaps near the sender's frame period) play
 * smoothly; bursts — a stall, then frames together — show as a few jumps between freezes. Not thread-safe:
 * the engine keeps it under its media lock.
 */
class VideoArrivalStats {
public:
  static constexpr int64_t kStallMs = 200;
  static constexpr int64_t kBunchedMs = 10;

  /** A frame numbered `seq` by its sender arrived at `now_ms` (any monotonic clock). */
  void Note(uint32_t seq, int64_t now_ms);
  VideoArrivalSummary Summary() const;

private:
  static constexpr int64_t kBucketMs = 5;
  static constexpr size_t kBuckets = 400; // 0–2 s in 5 ms steps; the last one takes the rest

  int64_t GapPercentile(double fraction) const;

  VideoArrivalSummary summary_;
  std::optional<uint32_t> last_seq_;
  std::optional<int64_t> last_arrival_ms_;
  std::array<uint64_t, kBuckets> gaps_{};
  uint64_t gap_count_ = 0;
};

} // namespace pbr
