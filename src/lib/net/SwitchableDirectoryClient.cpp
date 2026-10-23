#include "lib/net/SwitchableDirectoryClient.h"

#include <utility>

namespace pbr {

namespace {

Error NotConfigured() { return Error("Directory not configured"); }

} // namespace

void SwitchableDirectoryClient::SetBackend(std::shared_ptr<mesh::IDirectoryClient> backend) {
  std::lock_guard lock(mutex_);
  backend_ = std::move(backend);
}

bool SwitchableDirectoryClient::Configured() const { return Backend() != nullptr; }

std::shared_ptr<mesh::IDirectoryClient> SwitchableDirectoryClient::Backend() const {
  std::lock_guard lock(mutex_);
  return backend_;
}

Roe<std::vector<mesh::DirectoryHit>> SwitchableDirectoryClient::SearchPeople(const std::string& query) {
  const auto backend = Backend();
  if (!backend) {
    return NotConfigured();
  }
  return backend->SearchPeople(query);
}

Roe<mesh::DirectoryHit> SwitchableDirectoryClient::LookupRelayUser(const std::string& relay_user_id) {
  const auto backend = Backend();
  if (!backend) {
    return NotConfigured();
  }
  return backend->LookupRelayUser(relay_user_id);
}

Roe<mesh::DirectoryHit> SwitchableDirectoryClient::LookupByAccount(const std::string& account_id) {
  const auto backend = Backend();
  if (!backend) {
    return NotConfigured();
  }
  return backend->LookupByAccount(account_id);
}

Roe<std::vector<mesh::MeshNodeHit>> SwitchableDirectoryClient::ListMeshNodes() {
  const auto backend = Backend();
  if (!backend) {
    return NotConfigured();
  }
  return backend->ListMeshNodes();
}

} // namespace pbr
