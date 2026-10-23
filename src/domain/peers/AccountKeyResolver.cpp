#include "domain/peers/AccountKeyResolver.h"

#include "common/directory/DirectoryJson.h"
#include "lib/people/ContactIdentity.h"

#include <utility>

namespace pbr {

AccountKeyResolver::AccountKeyResolver(PeerSigningKeyStore& signing_keys, PeerKemKeyStore& kem_keys,
                                       PeerCardStore& cards, mesh::IDirectoryClient& directory)
    : signing_keys_(signing_keys), kem_keys_(kem_keys), cards_(cards), directory_(directory) {
  redirectLogger("AccountKeyResolver");
}

bool AccountKeyResolver::IsAccountKind(const std::string& kind) {
  return kind == ContactIdKindToString(mesh::ContactIdKind::Account);
}

void AccountKeyResolver::Remember(const PeerCardRecord& record) {
  const std::string kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  PeerSigningKeyRecord signing;
  signing.signing_public_key_b64 = record.keys.signing_public_key_b64;
  signing.source = record.source;
  signing.source_ref = record.keys.account_id;
  (void)signing_keys_.Put(kind, record.keys.account_id, std::move(signing));
  PeerKemKeyRecord kem;
  kem.kem_public_key_b64 = record.keys.kem_public_key_b64;
  kem.signing_public_key_b64 = record.keys.signing_public_key_b64;
  kem.binding_signature_b64 = record.keys.kem_binding_signature_b64;
  kem.source = record.source;
  kem.source_ref = record.keys.account_id;
  (void)kem_keys_.Put(kind, record.keys.account_id, std::move(kem));
}

void AccountKeyResolver::LoadKeptCards() {
  auto records = cards_.List();
  if (!records) {
    log().warning << "kept cards unavailable: " << records.error().message;
    return;
  }
  for (const PeerCardRecord& record : *records) {
    Remember(record);
  }
}

Roe<PeerCardRecord> AccountKeyResolver::AddDirectoryHit(const mesh::DirectoryHit& hit) {
  const auto account_id = PrimaryAccountIdFromHit(hit);
  if (!account_id) {
    return Error("Directory entry has no account");
  }
  if (!hit.signing_public_key_b64 || !hit.kem_public_key_b64 || !hit.kem_binding_signature_b64) {
    return Error("Directory entry carries no bound keys");
  }
  auto kept = cards_.AddKeyCard(KeyCard{.account_id = *account_id,
                                        .signing_public_key_b64 = *hit.signing_public_key_b64,
                                        .kem_public_key_b64 = *hit.kem_public_key_b64,
                                        .kem_binding_signature_b64 = *hit.kem_binding_signature_b64},
                                "directory");
  if (!kept) {
    return kept.error();
  }
  Remember(*kept);
  return kept;
}

Roe<PeerCardRecord> AccountKeyResolver::Resolve(const std::string& account_id) {
  auto known = cards_.Get(account_id);
  if (known && *known) {
    Remember(**known);
    return **known;
  }
  auto hit = directory_.LookupByAccount(account_id);
  if (!hit) {
    return Error("No keys for " + account_id + ": " + hit.error().message);
  }
  if (!hit->account_id || *hit->account_id != account_id) {
    return Error("Directory answered for another account");
  }
  return AddDirectoryHit(*hit);
}

Roe<PeerSigningKeyRecord> AccountKeyResolver::SigningView::Resolve(const std::string& kind, const std::string& value) {
  if (auto cached = owner_.signing_keys_.Get(kind, value)) {
    return *cached;
  }
  if (!IsAccountKind(kind)) {
    return Error("Peer signing key requires account identity");
  }
  if (auto resolved = owner_.Resolve(value); !resolved) {
    return resolved.error();
  }
  if (auto kept = owner_.signing_keys_.Get(kind, value)) {
    return *kept;
  }
  return Error("Signing key does not match the account");
}

Roe<PeerKemKeyRecord> AccountKeyResolver::KemView::Resolve(const std::string& kind, const std::string& value) {
  if (auto cached = owner_.kem_keys_.Get(kind, value)) {
    return *cached;
  }
  if (!IsAccountKind(kind)) {
    return Error("Peer KEM public key requires account identity");
  }
  if (auto resolved = owner_.Resolve(value); !resolved) {
    return resolved.error();
  }
  if (auto kept = owner_.kem_keys_.Get(kind, value)) {
    return *kept;
  }
  return Error("KEM public key is not bound to the account");
}

} // namespace pbr
