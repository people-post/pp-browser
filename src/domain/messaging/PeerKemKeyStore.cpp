#include "domain/messaging/PeerKemKeyStore.h"
#include "common/PbrCompat.h"

namespace pbr {

std::string PeerKemKeyStore::MakeKey(const std::string& kind, const std::string& value) {
  return kind + "\0" + value;
}

void PeerKemKeyStore::Put(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                          PeerKemKeyRecord record) {
  std::lock_guard lock(mutex_);
  keys_[MakeKey(peer_identity_kind, peer_identity_value)] = std::move(record);
}

std::optional<PeerKemKeyRecord> PeerKemKeyStore::Get(const std::string& peer_identity_kind,
                                                     const std::string& peer_identity_value) const {
  std::lock_guard lock(mutex_);
  const auto it = keys_.find(MakeKey(peer_identity_kind, peer_identity_value));
  if (it == keys_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void PeerKemKeyStore::Clear() {
  std::lock_guard lock(mutex_);
  keys_.clear();
}

PeerKemKeyResolver::PeerKemKeyResolver(PeerKemKeyStore& store) : store_(store) {}

Roe<PeerKemKeyRecord> PeerKemKeyResolver::Resolve(const std::string& peer_identity_kind,
                                                  const std::string& peer_identity_value) {
  if (auto cached = store_.Get(peer_identity_kind, peer_identity_value)) {
    return *cached;
  }
  return Error("Peer KEM public key not found");
}

} // namespace pbr
