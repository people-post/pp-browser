#pragma once

#include "common/Metrics.h"
#include "foundation/platform/os/OsPeakRss.h"

#include <chrono>
#include <cstdint>

namespace pbr {

/**
 * `attachment.transfer`: one line per large transfer, for the memory a send, a receive and an open really use.
 * `dir` is send / receive / open, `mb` the file size in whole megabytes, `peak_rss_mb` the process high-water
 * mark so far (start the app fresh to measure one operation), `ms` how long it took, `ok` 0 / 1. Sizes and
 * timings only: no names, ids or addresses. Only files of 1 MB and up are recorded.
 */
inline void EmitAttachmentTransferMetric(const char* dir, const uint64_t bytes, const int64_t ms, const bool ok) {
  if (bytes < 1000ULL * 1000ULL) {
    return;
  }
  MetricsLine("attachment.transfer")
      .Add("dir", dir)
      .Add("mb", bytes / (1000ULL * 1000ULL))
      .Add("peak_rss_mb", static_cast<uint64_t>(PeakResidentBytes() / (1000LL * 1000LL)))
      .Add("ms", ms)
      .Add("ok", ok)
      .Emit();
}

} // namespace pbr
