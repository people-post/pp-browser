#pragma once

#include "base/net/ServiceClients.h"
#include "base/people/ContactTypes.h"

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

  std::optional<DirectoryHit> Get(const std::string& relay_user_id) const;

  /** Cache hit returns immediately. Otherwise schedules LookupRelayUser on IO and refreshes UI. */
  void EnsureLookup(const std::string& relay_user_id);

  void Put(const DirectoryHit& hit);

private:
  IDirectoryClient& directory_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, DirectoryHit> by_relay_id_;
  std::unordered_set<std::string> inflight_;
  std::function<void()> on_updated_;
};

} // namespace pbr
