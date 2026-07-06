#include "feature/messaging/RelayDirectoryKemKeyResolver.h"

#include "base/messaging/MessagingJson.h"

namespace pbr {

RelayDirectoryKemKeyResolver::RelayDirectoryKemKeyResolver(PeerKemKeyStore& store, IDirectoryClient& directory)
    : store_(store), directory_(directory) {}

bool RelayDirectoryKemKeyResolver::IsRelayUserKind(const std::string& peer_identity_kind) {
  return peer_identity_kind == ContactIdKindToString(ContactIdKind::RelayUser) || peer_identity_kind == "relay_user";
}

Roe<PeerKemKeyRecord> RelayDirectoryKemKeyResolver::Resolve(const std::string& peer_identity_kind,
                                                            const std::string& peer_identity_value) {
  if (auto cached = store_.Get(peer_identity_kind, peer_identity_value)) {
    return *cached;
  }

  if (!IsRelayUserKind(peer_identity_kind)) {
    return Error("Peer KEM public key not found");
  }

  auto hit = directory_.LookupRelayUser(peer_identity_value);
  if (!hit) {
    return hit.error();
  }
  if (!hit->kem_public_key_b64 || hit->kem_public_key_b64->empty()) {
    return Error("Directory entry missing KEM public key");
  }

  PeerKemKeyRecord record;
  record.kem_public_key_b64 = *hit->kem_public_key_b64;
  record.source = "directory";
  record.source_ref = peer_identity_value;
  store_.Put(peer_identity_kind, peer_identity_value, record);
  return record;
}

} // namespace pbr
