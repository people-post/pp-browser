#pragma once

#include "base/net/ServiceClients.h"
#include "domain/people/ContactTypes.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pbr {

/** In-memory directory nicknames for stranger peers (no disk write). */
class DirectoryShadowCache {
public:
  explicit DirectoryShadowCache(IDirectoryClient& directory);

  void SetOnUpdated(std::function<void()> callback);
  /** Fired when a hit is cached (lookup or Put) — used to refresh initiation floors (P001). */
  void SetOnHitCached(std::function<void(const DirectoryHit&)> callback);

  /** Lookup by Account ID or `relay:` route id. */
  std::optional<DirectoryHit> Get(const std::string& identity) const;

  /**
   * Cache hit returns immediately. Otherwise schedules directory lookup on IO and refreshes UI.
   * `account:…` → LookupByAccount; `relay:…` → LookupRelayUser.
   */
  void EnsureLookup(const std::string& identity);

  void Put(const DirectoryHit& hit);

private:
  IDirectoryClient& directory_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, DirectoryHit> by_id_;
  std::unordered_set<std::string> inflight_;
  std::function<void()> on_updated_;
  std::function<void(const DirectoryHit&)> on_hit_cached_;
};

} // namespace pbr
