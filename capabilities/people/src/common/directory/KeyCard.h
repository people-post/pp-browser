#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The keys an account's peers need (docs/contracts/ACCOUNT_CARD.md#key-card): its signing key, which hashes to the
 * Account ID, and its KEM key with the account's binding (KEM binding v2, foundation/crypto/KemKeyBinding.h). Checked
 * by VerifyKeyCard (lib/people/AccountCard.h) without trusting whoever carried it.
 *
 * The keys are **typed** (docs/contracts/SIGNED_EXTENSIONS.md § Typed blobs: the algorithm id, then the key), in
 * standard base64, as the JSON carries them; the stores filled from a card (PeerSigningKeyStore, PeerKemKeyStore) hold
 * them the same way. Code that needs the raw key (`MlDsa`, `HybridKem::Encapsulate`, an identity's hash) reads it with
 * RawSigningPublicKey / RawKemPublicKey (lib/people/CardKeys.h). `kem_binding_b64` is the binding blob.
 */
struct KeyCard {
  std::string account_id;
  std::string signing_public_key_b64;
  std::string kem_public_key_b64;
  std::string kem_binding_b64;

  bool operator==(const KeyCard&) const = default;
};

/** Same account and keys (signatures are randomized, so two bindings of the same keys differ). */
inline bool SameKeys(const KeyCard& a, const KeyCard& b) {
  return a.account_id == b.account_id && a.signing_public_key_b64 == b.signing_public_key_b64 &&
         a.kem_public_key_b64 == b.kem_public_key_b64;
}

/** `{account_id, signing_public_key_b64, kem_public_key_b64, kem_binding_b64}`. */
Object KeyCardToJson(const KeyCard& card);
/** The four fields, each a non-empty string (what they hold is VerifyKeyCard's to check). */
Roe<KeyCard> KeyCardFromJson(const Object& json);

} // namespace pbr
