#pragma once

#include "common/directory/KeyCard.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/RelayId.h"
#include "foundation/crypto/SignedReceive.h"

#include "common/Error.h"
#include "common/ExtArea.h"
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
 * the device's envelopes) and the device's own KEM key (sessions with it open under that). The keys are typed, in
 * standard base64 (lib/people/CardKeys.h reads the raw key); one device's may be of another algorithm than another's
 * (an algorithm migration happens per device).
 */
struct AccountCardDevice {
  std::string peer_id;
  std::string signing_public_key_b64;
  std::string kem_public_key_b64;

  bool operator==(const AccountCardDevice&) const = default;
};

/**
 * A relay the account receives on (multi-relay MR002; release-compatibility SIGNED_FORMATS § 3.3): the HTTPS base URL a
 * client uses and the relay id whose certificate proves the audience a sender signs its send into that relay for
 * (RelayPins::ContactAudience). The same `{origin, relay_id}` a locator's relay entry and our relays name.
 */
using AccountCardRelay = RelayAddress;

/**
 * A key card plus what the account says about itself (nickname, its device list, its relays, its price for
 * strangers), signed by the account: card v5 (docs/contracts/ACCOUNT_CARD.md § Account card; SIGNED_FORMATS § 2.2).
 * The newest card (`issued_at`) is the account's word on all of them (M021, MR002).
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
  /**
   * The card's extension area (`ext_b64`): signed, the last field of its bytes, kept as received (unknown non-critical
   * entries ride a stored or forwarded card). Empty at release 1.
   */
  pp::ExtArea ext;
  /** The account's typed signature over AccountCardSignBytes. */
  std::string card_signature_b64;

  bool operator==(const AccountCard&) const = default;
};

/** `card_version` (and the byte after the domain): release 1's, the floor and the only one read. */
inline constexpr int64_t kAccountCardVersion = 5;
inline constexpr char kAccountCardDomain[] = "pp-browser:account-card";

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
/**
 * A device entry is well formed: its typed keys read (an unknown algorithm: AppError::NewerFormat) and its PeerId is
 * its raw signing key's.
 */
Roe<void> VerifyCardDevice(const AccountCardDevice& device);
/**
 * One relay origin is within MR004's bounds: an absolute `https://` URL of at most 128 bytes with a host, and no query,
 * fragment, credentials, spaces or control characters.
 */
bool IsCardRelayOrigin(const std::string& origin);
/**
 * A relay list is within MR004's bounds: at most 4 entries, each IsCardRelayOrigin with a relay id (not none), no two
 * the same relay by origin (scheme and host compared without case, `:443` as no port, an empty path as `/`) or by
 * relay id. A list breaking one is refused whole. (Whether the relay at an origin proves its relay id is the sender's
 * check, by the relay's certificate.)
 */
Roe<void> VerifyCardRelays(const std::vector<AccountCardRelay>& relays);
/** How far ahead of the receiver's clock a card's `issued_at` may be. */
inline constexpr int64_t kAccountCardClockSkewMs = 5 * 60 * 1000;

/** The card's JSON (`card_version` 5; `nickname` and `ext_b64` left out when empty). */
Object AccountCardToJson(const AccountCard& card);
/**
 * Steps 1–3 of the receiver's order (docs/contracts/SIGNED_EXTENSIONS.md § The receiver's order): `card_version` (none
 * or below 5: refused as an older app's; above: AppError::NewerFormat), the v5 fields and `ext_b64` (malformed:
 * refused), every typed key and the signature (an unknown algorithm: AppError::NewerFormat). Steps 4–6 are
 * VerifyAccountCard's: a card read here is not checked yet.
 */
Roe<AccountCard> AccountCardFromJson(const Object& json);
/**
 * The card reads as a newer app's (steps 1–3: a higher `card_version` or an unknown algorithm id): the owner is marked
 * as needing a newer app here, not asked for the card again and again (INVENTORY U2).
 */
bool IsNewerAccountCard(const Object& json);

/**
 * The key card checks out: the KEM binding v2 is received (foundation/crypto/KemKeyBinding.h) and the signing key
 * hashes to `account_id`. An update-needed binding or key is AppError::NewerFormat.
 */
Roe<void> VerifyKeyCard(const KeyCard& card);
/**
 * Steps 3–6 of the receiver's order for a card read (AccountCardFromJson) or made here: the typed blobs, the account's
 * signature over the rebuilt bytes, an unknown critical entry (AppError::NewerFormat), then the key card
 * (VerifyKeyCard), 1–5 distinct devices that check out (VerifyCardDevice), the relays and floor within their bounds,
 * and not from the future.
 */
Roe<void> VerifyAccountCard(const AccountCard& card, int64_t now_ms);
/**
 * The whole receiver's order for a card's JSON (AccountCardFromJson, then VerifyAccountCard): the verdict. Step 6's
 * refusals: the KEM binding's own verdict (ReceiveKemBinding), a signing key that is not the Account ID's as `Forged`,
 * the bounds and the clock as `Malformed`. `card` gets the card when accepted. The vector file and the tests read it.
 */
SignedReceipt ReceiveAccountCard(const Object& json, int64_t now_ms, AccountCard* card = nullptr);

/**
 * What the account signs (SIGNED_FORMATS § 2.2):
 *
 *   "pp-browser:account-card" 00 ‖ u8 5 ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed) ‖ LenUtf8 nickname
 *   ‖ u64 devices, by peer_id bytes: LenUtf8 peer_id ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed)
 *   ‖ u64 relays, in list order: LenUtf8 origin ‖ relay_id (32 raw) ‖ i64 initiation_floor ‖ i64 issued_at ‖ ext area
 *
 * Empty when a key is not base64 (whether it names a known algorithm is VerifyAccountCard's).
 */
ByteVector AccountCardSignBytes(const AccountCard& card);
/**
 * Signs `card` (all but its signature filled) with `sign`, the account key's raw signature in base64
 * (`IdentityStore::SignBytes`): the card's signature is typed.
 */
Roe<AccountCard> SignAccountCard(AccountCard card, const std::function<Roe<std::string>(const ByteVector&)>& sign);

} // namespace pbr
