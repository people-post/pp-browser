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

/** A relay the account receives on (multi-relay MR002): named by the HTTPS base URL a client uses. */
struct AccountCardRelay {
  std::string origin;

  bool operator==(const AccountCardRelay&) const = default;
};

/**
 * A key card plus what the account says about itself (nickname, its device list, its relays, its price for
 * strangers), signed by the account. The newest card (`issued_at`) is the account's word on all of them (M021, MR002).
 */
struct AccountCard {
  KeyCard keys;
  /** The account's own name for itself: a hint, never an identity. */
  std::string nickname;
  /** The account's devices, the issuing device first. */
  std::vector<AccountCardDevice> devices;
  /** The relays the account receives on, preferred first (MR002); empty: over the mesh only. */
  std::vector<AccountCardRelay> relays;
  /** The price for opening a chat or call with the account, `pp_credit` minor units (pricing P001, MR005). */
  int64_t initiation_floor = 0;
  int64_t issued_at = 0;
  std::string card_signature_b64;

  bool operator==(const AccountCard&) const = default;
};

/** At most this many devices per account (M021: every copy of a message costs one per device). */
inline constexpr size_t kMaxAccountCardDevices = 5;
/** Relay list bounds (MR004): at most this many relays, each origin at most this many bytes. */
inline constexpr size_t kMaxAccountCardRelays = 4;
inline constexpr size_t kMaxCardRelayOriginBytes = 128;
/** The largest floor a card may carry: the relay reads it as a JSON number, exact up to 2^53 - 1. */
inline constexpr int64_t kMaxCardInitiationFloor = (int64_t{1} << 53) - 1;

/** The card's entry for device `peer_id`; nullptr when the card does not list it. */
const AccountCardDevice* FindCardDevice(const AccountCard& card, const std::string& peer_id);
/** The PeerIds a card lists, in its order. */
std::vector<std::string> CardDevicePeerIds(const AccountCard& card);
/** A device entry is well formed: its keys decode to their sizes and its PeerId is its signing key's. */
Roe<void> VerifyCardDevice(const AccountCardDevice& device);
/**
 * One relay origin is within MR004's bounds: an absolute `https://` URL of at most 128 bytes with a host, and no query,
 * fragment, credentials, spaces or control characters.
 */
bool IsCardRelayOrigin(const std::string& origin);
/**
 * A relay list is within MR004's bounds: at most 4 entries, each IsCardRelayOrigin, no two the same relay (scheme and
 * host compared without case, `:443` as no port, an empty path as `/`). A list breaking one is refused whole.
 */
Roe<void> VerifyCardRelays(const std::vector<AccountCardRelay>& relays);
/** How far ahead of the receiver's clock a card's `issued_at` may be. */
inline constexpr int64_t kAccountCardClockSkewMs = 5 * 60 * 1000;

Object AccountCardToJson(const AccountCard& card);
Roe<AccountCard> AccountCardFromJson(const Object& json);

/** The signing key hashes to `account_id` and the KEM binding verifies. */
Roe<void> VerifyKeyCard(const KeyCard& card);
/**
 * The key card checks out, the account signed the card, devices are distinct and bounded, the relays and floor are
 * within their bounds, and it is not from the future.
 */
Roe<void> VerifyAccountCard(const AccountCard& card, int64_t now_ms);

/** What the account signs (domain pp-browser:account-card-v3); empty when a key does not decode to its size. */
ByteVector AccountCardSignBytes(const AccountCard& card);
/** Signs `card` (all but its signature filled) with `sign` (the account key, base64 out). */
Roe<AccountCard> SignAccountCard(AccountCard card, const std::function<Roe<std::string>(const ByteVector&)>& sign);

} // namespace pbr
