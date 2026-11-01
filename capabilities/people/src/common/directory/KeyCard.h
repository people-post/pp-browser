#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The keys an account's peers need (docs/contracts/ACCOUNT_CARD.md#key-card): its signing key, which
 * hashes to the Account ID, and its KEM key with the account's binding. Checked by VerifyKeyCard
 * (lib/messaging/AccountCard.h) without trusting whoever carried it.
 */
struct KeyCard {
  std::string account_id;
  std::string signing_public_key_b64;
  std::string kem_public_key_b64;
  std::string kem_binding_signature_b64;

  bool operator==(const KeyCard&) const = default;
};

/** Same account and keys (signatures are randomized, so two bindings of the same keys differ). */
inline bool SameKeys(const KeyCard& a, const KeyCard& b) {
  return a.account_id == b.account_id && a.signing_public_key_b64 == b.signing_public_key_b64 &&
         a.kem_public_key_b64 == b.kem_public_key_b64;
}

Object KeyCardToJson(const KeyCard& card);
Roe<KeyCard> KeyCardFromJson(const Object& json);

} // namespace pbr
