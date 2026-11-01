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

/**
 * One of the account's devices (M021): its PeerId, the device signing key that PeerId comes from (it signs
 * the device's envelopes) and the device's own KEM key (sessions with it open under that).
 */
struct AccountCardDevice {
  std::string peer_id;
  std::string signing_public_key_b64;
  std::string kem_public_key_b64;

  bool operator==(const AccountCardDevice&) const = default;
};

/**
 * A key card plus what the account says about itself (nickname, its device list), signed by the account.
 * The newest card (`issued_at`) is the account's device list (M021).
 */
struct AccountCard {
  KeyCard keys;
  /** The account's own name for itself: a hint, never an identity. */
  std::string nickname;
  /** The account's devices, the issuing device first. */
  std::vector<AccountCardDevice> devices;
  int64_t issued_at = 0;
  std::string card_signature_b64;

  bool operator==(const AccountCard&) const = default;
};

/** At most this many devices per account (M021: every copy of a message costs one per device). */
inline constexpr size_t kMaxAccountCardDevices = 5;

/** The card's entry for device `peer_id`; nullptr when the card does not list it. */
const AccountCardDevice* FindCardDevice(const AccountCard& card, const std::string& peer_id);
/** The PeerIds a card lists, in its order. */
std::vector<std::string> CardDevicePeerIds(const AccountCard& card);
/** A device entry is well formed: its keys decode to their sizes and its PeerId is its signing key's. */
Roe<void> VerifyCardDevice(const AccountCardDevice& device);
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
