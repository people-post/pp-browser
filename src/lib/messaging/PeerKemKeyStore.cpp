#include "lib/messaging/PeerKemKeyStore.h"

#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

std::string PeerKemKeyStore::MakeKey(const std::string& kind, const std::string& value) {
  return kind + '\0' + value;  // a char: the string literal "\0" is empty (no separator)
}

bool PeerKemKeyStore::Put(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                          PeerKemKeyRecord record) {
  if (peer_identity_kind != ContactIdKindToString(mesh::ContactIdKind::Account) ||
      !VerifyKemBinding(peer_identity_value, record.signing_public_key_b64, record.kem_public_key_b64,
                        record.binding_signature_b64)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  keys_[MakeKey(peer_identity_kind, peer_identity_value)] = std::move(record);
  return true;
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
