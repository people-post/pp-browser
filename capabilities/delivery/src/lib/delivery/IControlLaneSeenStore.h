#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Control-lane ids already applied, per sender: a lane message is applied at most once — a relay redelivery, a
 * second transport or a replay within the freshness window is dropped. Any thread. The app's storage keeps them
 * (CR024); tests use an in-memory one.
 */
class IControlLaneSeenStore {
public:
  /** Kept longer than the lane's signed-time bound (an hour either way), so a replay is a duplicate. */
  static constexpr int64_t kRetentionMs = 3 * 60 * 60 * 1000;

  virtual ~IControlLaneSeenStore() = default;

  /** True the first time (`sender`, `control_id`) is seen (recorded now); false for a repeat. */
  virtual Roe<bool> FirstSight(const std::string& sender, const std::string& control_id, int64_t now_ms) = 0;
  /** Undo a first sight whose apply failed, so a redelivery of it can still apply. */
  virtual void Forget(const std::string& sender, const std::string& control_id) = 0;
};

} // namespace pbr
