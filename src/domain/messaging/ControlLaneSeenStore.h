#pragma once

#include "common/Error.h"

#include <cstdint>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * Control-lane ids already applied, per sender (profile.db `control_lane_seen`): a lane message is
 * applied at most once — a relay redelivery, a second transport or a replay within the freshness
 * window is dropped. Rows older than the retention outlive every window and are pruned. Any thread.
 */
class ControlLaneSeenStore {
public:
  /** Kept longer than the lane's signed-time bound (an hour either way), so a replay is a duplicate. */
  static constexpr int64_t kRetentionMs = 3 * 60 * 60 * 1000;

  explicit ControlLaneSeenStore(std::string profile_db_path);

  /** True the first time (`sender`, `control_id`) is seen (recorded now); false for a repeat. */
  Roe<bool> FirstSight(const std::string& sender, const std::string& control_id, int64_t now_ms);

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
  int64_t last_prune_ms_ = 0;
};

} // namespace pbr
