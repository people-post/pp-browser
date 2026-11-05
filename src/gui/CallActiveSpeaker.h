#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pbr {

/**
 * Who is speaking in a group call, for its spotlight (V059): the loudest peer that has spoken for a moment takes the
 * spotlight, keeps it a few seconds, and gives it up only once quiet while someone else speaks — no flicker between
 * two talking at once. When everyone is quiet the last speaker stays. A user's tap or "back to grid" wins over it.
 */
class CallActiveSpeaker {
public:
  /** A peer's smoothed audio level (MediaStreamHealth::peak_level, 0..1) at which it counts as speaking. */
  static constexpr float kSpeakingLevel = 0.08f;
  /** Speaking this long takes the spotlight. */
  static constexpr int64_t kSpeakForMs = 600;
  /** The spotlit speaker keeps it at least this long. */
  static constexpr int64_t kHoldMs = 3000;

  /** Each peer's stream_id and level now; returns the spotlit speaker's stream_id (empty: none yet). */
  const std::string& Update(const std::vector<std::pair<std::string, float>>& levels, const int64_t now_ms) {
    std::unordered_map<std::string, int64_t> speaking;
    std::string loudest;
    float loudest_level = 0.f;
    for (const auto& [stream, level] : levels) {
      if (level < kSpeakingLevel) {
        continue;
      }
      const auto it = speaking_since_.find(stream);
      const int64_t since = it != speaking_since_.end() ? it->second : now_ms;
      speaking.emplace(stream, since);
      if (now_ms - since >= kSpeakForMs && level > loudest_level) {
        loudest = stream;
        loudest_level = level;
      }
    }
    speaking_since_ = std::move(speaking);
    if (loudest.empty() || loudest == current_) {
      return current_;
    }
    const bool current_speaking = speaking_since_.count(current_) > 0;
    if (current_.empty() || (now_ms - current_since_ms_ >= kHoldMs && !current_speaking)) {
      current_ = loudest;
      current_since_ms_ = now_ms;
    }
    return current_;
  }

  const std::string& Current() const { return current_; }

  void Reset() {
    speaking_since_.clear();
    current_.clear();
    current_since_ms_ = 0;
  }

private:
  std::unordered_map<std::string, int64_t> speaking_since_;
  std::string current_;
  int64_t current_since_ms_ = 0;
};

} // namespace pbr
