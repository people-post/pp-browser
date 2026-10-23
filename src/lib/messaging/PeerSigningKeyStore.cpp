#include "lib/messaging/PeerSigningKeyStore.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "crypto/PeerId.h"
#include "common/directory/DirectoryJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

std::string PeerSigningKeyStore::MakeKey(const std::string& kind, const std::string& value) {
  return kind + '\0' + value;
}

bool PeerSigningKeyStore::Put(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                              PeerSigningKeyRecord record) {
  auto public_key = Base64Decode(record.signing_public_key_b64);
  if (!public_key) {
    return false;
  }
  // Both self-certifying identities: an Account ID hashes the account key, a PeerId the device key.
  Roe<std::string> derived = Error("Not a self-certifying identity kind");
  if (peer_identity_kind == ContactIdKindToString(mesh::ContactIdKind::Account)) {
    derived = AccountIdFromMlDsaPublicKey(*public_key);
  } else if (peer_identity_kind == ContactIdKindToString(mesh::ContactIdKind::PeerId)) {
    derived = pp::PeerIdFromMlDsaPublicKey(*public_key);
  }
  if (!derived || *derived != peer_identity_value) {
    return false;
  }
  std::lock_guard lock(mutex_);
  keys_[MakeKey(peer_identity_kind, peer_identity_value)] = std::move(record);
  return true;
}

void PeerSigningKeyStore::PutUncheckedForTest(const std::string& peer_identity_kind,
                                              const std::string& peer_identity_value, PeerSigningKeyRecord record) {
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
