#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The calls this device learned are over (P017): ended here, or an invite read too late to ring or for a call
 * already ended. The phone's call screen ends a push's call waiting for one of them at once. Newest last, the last
 * kKept; `seq` counts every one, so a reader on another thread takes each once (NewSince). Calls owner writes.
 */
struct RecentCallsOver {
  static constexpr std::size_t kKept = 8;

  std::vector<std::string> ids;
  uint64_t seq = 0;

  void Note(const std::string& call_id);
  /** The ids noted after a reader's `seen` (its last `seq`), oldest first; at most kKept of them. */
  std::vector<std::string> NewSince(uint64_t seen) const;
};

} // namespace pbr
