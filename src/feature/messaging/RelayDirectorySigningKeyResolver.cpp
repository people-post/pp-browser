#include "feature/messaging/RelayDirectorySigningKeyResolver.h"

#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/PbrCompat.h"

namespace pbr {

RelayDirectorySigningKeyResolver::RelayDirectorySigningKeyResolver(PeerSigningKeyStore& store,
                                                                 IDirectoryClient& directory)
    : store_(store), directory_(directory) {}

bool RelayDirectorySigningKeyResolver::IsAccountKind(const std::string& peer_identity_kind) {
  return peer_identity_kind == ContactIdKindToString(ContactIdKind::Account) || peer_identity_kind == "account";
}

Roe<PeerSigningKeyRecord> RelayDirectorySigningKeyResolver::Resolve(const std::string& peer_identity_kind,
                                                                    const std::string& peer_identity_value) {
  if (auto cached = store_.Get(peer_identity_kind, peer_identity_value)) {
    return *cached;
  }

  if (!IsAccountKind(peer_identity_kind)) {
    return Error("Peer signing key requires account identity");
  }

  auto hit = directory_.LookupByAccount(peer_identity_value);
  if (!hit) {
    return hit.error();
  }
  if (!hit->signing_public_key_b64 || hit->signing_public_key_b64->empty()) {
    return Error("Directory entry missing signing public key");
  }

  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = *hit->signing_public_key_b64;
  record.source = "directory";
  record.source_ref = peer_identity_value;
  store_.Put(peer_identity_kind, peer_identity_value, record);
  return record;
}

} // namespace pbr
