#include "lib/media/VideoArrivalStats.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

void VideoArrivalStats::Note(const uint32_t seq, const int64_t now_ms) {
  ++summary_.frames;
  if (last_seq_ && seq > *last_seq_ + 1) {
    summary_.lost += seq - *last_seq_ - 1;
  }
  if (!last_seq_ || seq > *last_seq_) {
    last_seq_ = seq;
  }
  if (last_arrival_ms_) {
    const int64_t gap = std::max<int64_t>(0, now_ms - *last_arrival_ms_);
    const size_t bucket = std::min<size_t>(static_cast<size_t>(gap / kBucketMs), kBuckets - 1);
    ++gaps_[bucket];
    ++gap_count_;
    summary_.gap_max_ms = std::max(summary_.gap_max_ms, gap);
    if (gap >= kStallMs) {
      ++summary_.stalls;
    }
    if (gap < kBunchedMs) {
      ++summary_.bunched;
    }
  }
  last_arrival_ms_ = now_ms;
}

int64_t VideoArrivalStats::GapPercentile(const double fraction) const {
  if (gap_count_ == 0) {
    return 0;
  }
  const auto wanted = static_cast<uint64_t>(fraction * static_cast<double>(gap_count_ - 1)) + 1;
  uint64_t seen = 0;
  for (size_t i = 0; i < kBuckets; ++i) {
    seen += gaps_[i];
    if (seen >= wanted) {
      return static_cast<int64_t>(i) * kBucketMs;
    }
  }
  return static_cast<int64_t>(kBuckets - 1) * kBucketMs;
}

VideoArrivalSummary VideoArrivalStats::Summary() const {
  VideoArrivalSummary out = summary_;
  out.gap_p50_ms = GapPercentile(0.50);
  out.gap_p95_ms = GapPercentile(0.95);
  return out;
}

} // namespace pbr
