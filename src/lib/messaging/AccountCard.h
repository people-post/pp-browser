#pragma once

#include "common/directory/KeyCard.h"
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
