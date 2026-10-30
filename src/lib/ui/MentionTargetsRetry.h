#pragma once

#include <cstdint>

namespace pbr {

/** How long to wait before asking again whether a chat is a group (the first answer was "could not tell"). */
inline constexpr int64_t kMentionTargetsRetryGapMs = 1000;

/**
 * Remembers that the group check could not be answered (facade not ready, roster read failed) so the composer
 * asks again later, at most once per kMentionTargetsRetryGapMs. A definite answer (group or not) ends it.
 */
class MentionTargetsRetry {
public:
  /** Call after every check with its outcome. */
  void Record(const bool unknown, const int64_t now_ms) {
    pending_ = unknown;
    next_try_ms_ = now_ms + kMentionTargetsRetryGapMs;
  }
  bool Due(const int64_t now_ms) const { return pending_ && now_ms >= next_try_ms_; }

private:
  bool pending_ = false;
  int64_t next_try_ms_ = 0;
};

} // namespace pbr
