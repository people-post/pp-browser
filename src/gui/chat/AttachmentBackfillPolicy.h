#pragma once

#include <chrono>

namespace pbr {

/**
 * When a display sync should also run the attachment backfill (EnsureThreadAttachments), which reads the
 * whole thread and re-queues downloads that are missing or failed. Always when a thread is opened; while
 * it stays open at most once per kAttachmentBackfillInterval, so a transient failure (peer offline, relay
 * blip) is retried without a tap, but not on every message.
 */
inline constexpr std::chrono::seconds kAttachmentBackfillInterval{60};

inline bool ShouldBackfillAttachments(const bool thread_changed, const std::chrono::steady_clock::time_point now,
                                      const std::chrono::steady_clock::time_point last_backfill) {
  return thread_changed || now - last_backfill >= kAttachmentBackfillInterval;
}

} // namespace pbr
