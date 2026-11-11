#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * How long a peer stays warm after the last call or message with it (this device's clock). A warm peer's link
 * keeps a keepalive cadence — through a relay, the relay link under its carrier too (pp-cpp-amp v5.5.0) — so a
 * call a few minutes later reuses it instead of building a new circuit (#577). Past it the mark is cleared: a
 * carrier held forever to everyone ever called costs a keepalive and a relay splice each.
 */
inline constexpr int64_t kRecentPeerWarmMs = 10 * 60 * 1000;

/**
 * Which peers are warm and until when: the timing of the chat / call warm mark, without the mark itself. Not
 * thread-safe (its owner locks).
 */
class RecentPeerWarmth {
public:
  explicit RecentPeerWarmth(int64_t keep_ms = kRecentPeerWarmMs) : keep_ms_(keep_ms) {}

  /** A call with `peer_key`, or the user opening its chat: warm from `now_ms` on. True: it was not warm. */
  bool Warm(const std::string& peer_key, int64_t now_ms);
  /** A message to or from a warm peer keeps it warm from `now_ms`; a cold one stays cold. True: it was warm. */
  bool Touch(const std::string& peer_key, int64_t now_ms);
  /** The peers with no activity for the whole spell by `now_ms`, forgotten here: clear their marks. */
  std::vector<std::string> Lapsed(int64_t now_ms);
  bool IsWarm(const std::string& peer_key) const { return last_active_ms_.contains(peer_key); }
  void Clear() { last_active_ms_.clear(); }

private:
  int64_t keep_ms_;
  std::unordered_map<std::string, int64_t> last_active_ms_;
};

} // namespace pbr
