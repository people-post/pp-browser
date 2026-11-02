#pragma once

#include "lib/delivery/IControlLaneSeenStore.h"

#include <cstdint>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** IControlLaneSeenStore on profile.db (`control_lane_seen`); rows past the retention are pruned. */
class SqliteControlLaneSeenStore : public IControlLaneSeenStore {
public:
  explicit SqliteControlLaneSeenStore(std::string profile_db_path);

  Roe<bool> FirstSight(const std::string& sender, const std::string& control_id, int64_t now_ms) override;
  void Forget(const std::string& sender, const std::string& control_id) override;

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
  int64_t last_prune_ms_ = 0;
};

} // namespace pbr
