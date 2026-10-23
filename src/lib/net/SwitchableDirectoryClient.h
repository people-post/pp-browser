#pragma once

#include "mesh/types/IDirectoryClient.h"

#include <memory>
#include <mutex>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The profile's one directory client: a stable object the resolvers and caches keep, forwarding to
 * whichever backend the configuration names (HTTP, failover over several) — or to none, when no
 * directory is configured and every lookup answers "Directory not configured". Swapping the backend
 * (a config change) never leaves a holder with a freed client; a call already running keeps the old
 * backend alive until it returns. Thread-safe.
 */
class SwitchableDirectoryClient : public mesh::IDirectoryClient {
public:
  /** `backend` may be null: no directory. */
  void SetBackend(std::shared_ptr<mesh::IDirectoryClient> backend);
  bool Configured() const;

  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query) override;
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override;
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override;
  Roe<std::vector<mesh::MeshNodeHit>> ListMeshNodes() override;

private:
  std::shared_ptr<mesh::IDirectoryClient> Backend() const;

  mutable std::mutex mutex_;
  std::shared_ptr<mesh::IDirectoryClient> backend_;
};

} // namespace pbr
