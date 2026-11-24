#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/RelayId.h"

#include "common/Error.h"
#include "common/ExtArea.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Linking a device, as a sealed QR handshake (docs/contracts/LINK_DEVICE.md; multi-device M027). Two payloads, each
 * one QR code (binary mode) or the same bytes as text:
 *
 *   offer (the new device → the main device): u8 1 ‖ LenBytes KEM public key (typed, one-time) ‖ ext area
 *   reply (the main device → the new device): u8 1 ‖ LenBytes KEM ciphertext (typed) ‖ LenBytes AEAD sealed (typed)
 *
 * The reply's AEAD key is HKDF-SHA256 of the KEM shared secret (salt `pp-browser-link-v1`, info `link-reply-key`); its
 * AAD is `pp-browser-link-reply-v1` 00 ‖ u8 version ‖ LenBytes offer ‖ LenBytes KEM ciphertext (typed), so a reply
 * opens only for the offer it answers. Sealed inside, the bundle:
 *
 *   Raw(32) recovery seed ‖ u8 intent (1 = join) ‖ i64 expires_at_ms ‖ u8 relays (0 or 1)
 *     [‖ LenUtf8 origin ‖ Raw(32) relay id] ‖ ext area
 *
 * Nothing else is sent: the account's keys derive from the seed (M023 §2), its card and nickname come from our relays,
 * and the new device makes its own DEK. Both payloads are safe to show and paste anywhere: the offer is a public key,
 * the reply opens only with the one-time secret key that never leaves the new device.
 *
 * Both devices show the same 4-digit code, a hash of the offer (LinkConfirmationCode): the main device asks the person
 * to check it against the new device's before it seals, so a swapped offer is caught before the seed goes out.
 */
inline constexpr uint8_t kLinkOfferVersion = 1;
inline constexpr uint8_t kLinkReplyVersion = 1;
/** The text forms: a prefix, then the payload in base64url without padding. */
inline constexpr char kLinkOfferTextPrefix[] = "pp-browser-link-offer:";
inline constexpr char kLinkReplyTextPrefix[] = "pp-browser-link-reply:";
/**
 * A payload's first byte (its version) stays below this: a scanned QR code of text (a printable first byte) is never
 * read as a payload of a newer version.
 */
inline constexpr uint8_t kLinkPayloadVersionLimit = 0x20;
/** The most bytes a payload may have: a QR code's binary capacity at error correction M (version 40). */
inline constexpr size_t kMaxLinkPayloadBytes = 2331;
/** The most bytes of a relay origin in the bundle (the card's bound, MR004). */
inline constexpr size_t kMaxLinkRelayOriginBytes = 128;
/** How long a link takes at most: the new device's key and the bundle's expiry (15 minutes). */
inline constexpr int64_t kLinkWindowMs = 15 * 60 * 1000;

/** Link-specific refusals (`Error::category`), apart from AppError::NewerFormat ("update needed"). */
inline constexpr int32_t kLinkErrorCategory = 0x6c6e6b; // "lnk"
enum class LinkError : int32_t {
  /** Not an offer / reply at all, or one that does not parse. */
  Unreadable = 1,
  /** A reply this device's one-time key does not open (another device's, altered, or for an older offer). */
  NotForThisDevice = 2,
  /** The bundle's time ran out, or this device's one-time key did. */
  Expired = 3,
};
Error MakeLinkError(LinkError code, const std::string& detail);
bool IsLinkError(const Error& error, LinkError code);

/** What the bundle asks the new device to do: join the main device's device list (M021 d4). */
enum class LinkIntent : uint8_t {
  Join = 1,
};

/** The new device's one-time ML-KEM-768 key pair. The secret half is zeroed when it goes. Move only. */
class LinkOfferKeys {
public:
  LinkOfferKeys() = default;
  LinkOfferKeys(ByteVector public_key, ByteVector secret_key);
  ~LinkOfferKeys();
  LinkOfferKeys(LinkOfferKeys&& other) noexcept;
  LinkOfferKeys& operator=(LinkOfferKeys&& other) noexcept;
  LinkOfferKeys(const LinkOfferKeys&) = delete;
  LinkOfferKeys& operator=(const LinkOfferKeys&) = delete;

  /** A fresh pair (OS randomness). */
  static Roe<LinkOfferKeys> Generate();

  const ByteVector& public_key() const { return public_key_; }
  const ByteVector& secret_key() const { return secret_key_; }
  bool empty() const { return secret_key_.empty(); }
  /** Zero and drop both halves. */
  void Wipe();

private:
  ByteVector public_key_;
  ByteVector secret_key_;
};

/** What the reply seals. The seed is zeroed when it goes. */
struct LinkBundle {
  LinkBundle() = default;
  ~LinkBundle();
  LinkBundle(LinkBundle&& other) noexcept;
  LinkBundle& operator=(LinkBundle&& other) noexcept;
  LinkBundle(const LinkBundle&) = delete;
  LinkBundle& operator=(const LinkBundle&) = delete;

  /** The account's 32-byte recovery seed. */
  ByteVector seed;
  LinkIntent intent = LinkIntent::Join;
  int64_t expires_at_ms = 0;
  /** The account's preferred relay, when it is not the deployment's (the new device asks it for the card first). */
  std::optional<RelayAddress> relay;
};

/** The offer's bytes for the new device's one-time public key (raw ML-KEM-768, 1184 bytes). */
Roe<ByteVector> LinkOfferBytes(std::span<const uint8_t> kem_public_key, const pp::ExtArea& ext = {});

/**
 * The receiver's order for an offer (SIGNED_EXTENSIONS): the version (none, 0: Unreadable; ≥ 2: update needed), the
 * fields (Unreadable), the key's algorithm id (unknown: update needed), the area's critical entries (update needed).
 * The raw KEM public key on success.
 */
Roe<ByteVector> ReadLinkOffer(std::span<const uint8_t> offer);

/** The 4 digits both devices show for `offer`: BLAKE2b-256(`pp-browser-link-code-v1` 00 ‖ offer), first u32 mod 10⁴. */
std::string LinkConfirmationCode(std::span<const uint8_t> offer);

/**
 * The main device's reply to `offer` (read first: ReadLinkOffer's errors), sealing `bundle` to its key. An error when
 * the bundle is not a 32-byte seed, has no expiry, or names a relay out of bounds.
 */
Roe<ByteVector> SealLinkReply(std::span<const uint8_t> offer, const LinkBundle& bundle, const pp::ExtArea& ext = {});

/**
 * The new device opens `reply` to its `offer` with its one-time `keys`: the version (update needed above 1), the
 * fields and algorithm ids, the AEAD (NotForThisDevice), the bundle's fields and its area's critical entries (update
 * needed), an intent this build does not know (update needed), and the expiry against `now_ms` (Expired).
 */
Roe<LinkBundle> OpenLinkReply(std::span<const uint8_t> offer, const LinkOfferKeys& keys,
                              std::span<const uint8_t> reply, int64_t now_ms);

/** A payload as text: `prefix` then base64url without padding. */
std::string LinkPayloadText(std::string_view prefix, std::span<const uint8_t> payload);

/**
 * The payload in what was scanned or pasted: the text form after `prefix`, anywhere in `input` (up to the first
 * character that is not base64url), or a scanned binary payload as it is (a first byte below kLinkPayloadVersionLimit).
 * nullopt when `input` holds neither.
 */
std::optional<ByteVector> FindLinkPayload(std::string_view prefix, std::string_view input);

} // namespace pbr
