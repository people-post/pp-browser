#include "base/messaging/PeerSigningKeyStore.h"
#include "common/PbrCompat.h"

namespace pbr {

std::string PeerSigningKeyStore::MakeKey(const std::string& kind, const std::string& value) {
  return kind + '\0' + value;
}

void PeerSigningKeyStore::Put(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                              PeerSigningKeyRecord record) {
  std::lock_guard lock(mutex_);
  keys_[MakeKey(peer_identity_kind, peer_identity_value)] = std::move(record);
}

std::optional<PeerSigningKeyRecord> PeerSigningKeyStore::Get(const std::string& peer_identity_kind,
                                                             const std::string& peer_identity_value) const {
  std::lock_guard lock(mutex_);
  const auto it = keys_.find(MakeKey(peer_identity_kind, peer_identity_value));
  if (it == keys_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void PeerSigningKeyStore::Clear() {
  std::lock_guard lock(mutex_);
  keys_.clear();
}

PeerSigningKeyResolver::PeerSigningKeyResolver(PeerSigningKeyStore& store) : store_(store) {}

Roe<PeerSigningKeyRecord> PeerSigningKeyResolver::Resolve(const std::string& peer_identity_kind,
                                                          const std::string& peer_identity_value) {
  const auto record = store_.Get(peer_identity_kind, peer_identity_value);
  if (!record) {
    return Error("Peer signing key not found");
  }
  return *record;
}

} // namespace pbr
