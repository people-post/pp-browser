#pragma once

#include "lib/messaging/RecentPeerWarmth.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The chat / call warm mark on a peer's link, for kRecentPeerWarmMs after the last call or message with it: a
 * call, or the user opening the chat, marks it (Warm); a message either way keeps a warm peer warm (Touch); the
 * 1 s hub tick clears the marks whose spell ran out (Tick). The timing is RecentPeerWarmth's; this holds it under
 * a lock (calls, chat and the tick come from different threads) and calls the mesh outside it.
 */
class PeerWarmMarks {
public:
  using PeerFn = std::function<void(const std::string& peer_key)>;

  explicit PeerWarmMarks(int64_t keep_ms = kRecentPeerWarmMs) : warmth_(keep_ms) {}

  /** `mark` warms a peer's link (Amp MarkWarm), `clear` takes the mark off (Amp ClearWarm); empty: not wired. */
  void Bind(PeerFn mark, PeerFn clear);
  /** Unbind and forget every peer: the links they marked are gone with the mesh. */
  void Reset();

  void Warm(const std::string& peer_key, int64_t now_ms);
  void Touch(const std::string& peer_key, int64_t now_ms);
  void Tick(int64_t now_ms);
  bool IsWarm(const std::string& peer_key) const;

private:
  mutable std::mutex mu_;
  RecentPeerWarmth warmth_;
  PeerFn mark_;
  PeerFn clear_;
};

} // namespace pbr
