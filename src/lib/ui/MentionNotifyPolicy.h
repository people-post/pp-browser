#pragma once

#include <cstdint>
#include <optional>

namespace pbr {

/** A muted group still tells its members about an @mention, but at most once per this long (G015). */
inline constexpr int64_t kMentionNotifyMinGapMs = 60 * 1000;

/**
 * Whether a background message raises an OS notification.
 * Notifications off: never. Not muted: always. Muted: only an @mention of this account, and only when the
 * last mention notification for this thread is at least a minute old (`last_mention_notify_ms`: none yet).
 * The caller records `now_ms` as the new last time when this returns true for a muted thread.
 */
inline bool ShouldNotifyBackgroundMessage(const bool notifications_on, const bool thread_muted, const bool mentioned,
                                          const std::optional<int64_t> last_mention_notify_ms, const int64_t now_ms) {
  if (!notifications_on) {
    return false;
  }
  if (!thread_muted) {
    return true;
  }
  if (!mentioned) {
    return false;
  }
  return !last_mention_notify_ms || now_ms - *last_mention_notify_ms >= kMentionNotifyMinGapMs;
}

} // namespace pbr
