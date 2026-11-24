#pragma once

#include "foundation/platform/BackendTable.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * One feature the active backend's table serves, as the settings model lists it (profile-model PM002): the entry is
 * always in the model; when the table names no address for it, `available` is false and `reason` says so in plain words.
 * Nothing is hidden and nothing falls back to another backend's address.
 */
struct BackendServiceEntry {
  /** `ai`, `ai_key`, `people`, `feeds`, `crash_reports`, `account_backup`, `support_info`. */
  std::string id;
  std::string title;
  bool available = true;
  /** Why the feature is off ("" when available). */
  std::string reason;
};

/**
 * The entries for `table` in the user's language. `own_feed_set`: the user has a promoted-MCP address of their own
 * (it does not need the backend's).
 */
std::vector<BackendServiceEntry> BackendServiceEntries(const BackendTable& table, bool own_feed_set);

} // namespace pbr
