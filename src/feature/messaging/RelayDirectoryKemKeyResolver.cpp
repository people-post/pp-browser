#include "feature/messaging/RelayDirectoryKemKeyResolver.h"

#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/PbrCompat.h"

namespace pbr {

RelayDirectoryKemKeyResolver::RelayDirectoryKemKeyResolver(PeerKemKeyStore& store, IDirectoryClient& directory)
    : store_(store), directory_(directory) {}

bool RelayDirectoryKemKeyResolver::IsAccountKind(const std::string& peer_identity_kind) {
  return peer_identity_kind == ContactIdKindToString(ContactIdKind::Account) || peer_identity_kind == "account";
}

Roe<PeerKemKeyRecord> RelayDirectoryKemKeyResolver::Resolve(const std::string& peer_identity_kind,
                                                            const std::string& peer_identity_value) {
  if (auto cached = store_.Get(peer_identity_kind, peer_identity_value)) {
    return *cached;
  }

  if (!IsAccountKind(peer_identity_kind)) {
    return Error("Peer KEM public key requires account identity");
  }

  auto hit = directory_.LookupByAccount(peer_identity_value);
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
