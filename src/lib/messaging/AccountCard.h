#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "common/Value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The keys an account's peers need (docs/contracts/ACCOUNT_CARD.md): its signing key, which hashes to
 * the Account ID, and its KEM key with the account's binding. Valid without trusting whoever carried it.
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

/** A key card plus what the account says about itself (nickname, devices), signed by the account. */
struct AccountCard {
  KeyCard keys;
  /** The account's own name for itself: a hint, never an identity. */
  std::string nickname;
  /** PeerIds of the account's devices the issuing device knows, itself first. */
  std::vector<std::string> devices;
  int64_t issued_at = 0;
  std::string card_signature_b64;

  bool operator==(const AccountCard&) const = default;
};

inline constexpr size_t kMaxAccountCardDevices = 16;
/** How far ahead of the receiver's clock a card's `issued_at` may be. */
inline constexpr int64_t kAccountCardClockSkewMs = 5 * 60 * 1000;

Object KeyCardToJson(const KeyCard& card);
Roe<KeyCard> KeyCardFromJson(const Object& json);
Object AccountCardToJson(const AccountCard& card);
Roe<AccountCard> AccountCardFromJson(const Object& json);

/** The signing key hashes to `account_id` and the KEM binding verifies. */
Roe<void> VerifyKeyCard(const KeyCard& card);
/** The key card checks out, the account signed the card, devices are distinct and bounded, and it is not from the future. */
Roe<void> VerifyAccountCard(const AccountCard& card, int64_t now_ms);

/** What the account signs (domain pp-browser:account-card-v1); empty when a key does not decode to its size. */
ByteVector AccountCardSignBytes(const AccountCard& card);
/** Signs `card` (all but its signature filled) with `sign` (the account key, base64 out). */
Roe<AccountCard> SignAccountCard(AccountCard card, const std::function<Roe<std::string>(const ByteVector&)>& sign);

} // namespace pbr
