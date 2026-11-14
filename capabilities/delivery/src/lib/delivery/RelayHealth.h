#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Which relays failed us lately (multi-relay R2, sending in list order): a relay a send could not reach (a transport
 * failure or a 5xx) is tried after the healthy ones for kRelayFailedForMs; a send it takes clears it. A relay's
 * answer about the recipient (not registered, a price, a quota) says nothing about its health. By origin
 * (RelayOriginKey). Thread-safe.
 */
class RelayHealth {
public:
  /** How long a failed relay goes to the back of the list. */
  static constexpr int64_t kRelayFailedForMs = 5 * 60 * 1000;
  /** Relays remembered at most (the oldest failure is forgotten first). */
  static constexpr size_t kMaxRemembered = 64;

  /** `origins` in their order, the ones that failed within kRelayFailedForMs after the others (each group in order). */
  std::vector<std::string> Order(const std::vector<std::string>& origins, int64_t now_ms) const;
  void NoteFailed(const std::string& origin, int64_t now_ms);
  void NoteOk(const std::string& origin);
  bool FailedRecently(const std::string& origin, int64_t now_ms) const;

private:
  bool FailedLocked(const std::string& key, int64_t now_ms) const;

  mutable std::mutex mutex_;
  /** By RelayOriginKey: when it last failed. */
  std::unordered_map<std::string, int64_t> failed_at_;
};

} // namespace pbr
