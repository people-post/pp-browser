#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** `relay:` ‖ base64url-unpadded(32 bytes): 6 + 43 characters (release-compatibility SIGNED_FORMATS § 3.1, Q5). */
inline constexpr char kRelayIdPrefix[] = "relay:";
inline constexpr size_t kRelayIdBytes = 32;
inline constexpr size_t kRelayIdTextLength = 49;
/** The hash's domain and version: `BLAKE2b-256("pp-browser:relay-id" 00 ‖ u8 1 ‖ OperatorSet_founding)`. */
inline constexpr char kRelayIdDomain[] = "pp-browser:relay-id";
inline constexpr uint8_t kRelayIdVersion = 1;

/**
 * A relay's identity (SIGNED_FORMATS § 3; docs/contracts/SERVICE_ENDPOINTS.md § Relay identity): the BLAKE2b-256 hash
 * of its founding operator set, the offline keys that certify the relay's transport and signing keys. What clients pin
 * and what every carrier names instead of a PeerId: account cards' and locators' `relays`, relay links
 * (`https://host/api/relay#relay:…`), our relays, the locker, the build, pp-node's `registration.relay_id`. It never
 * changes for the relay (an operator-set change keeps it); a new PeerId is a new certificate, not a new relay.
 *
 * Written `relay:` + base64url without padding (49 characters), as the Account ID is `account:` + the same encoding.
 * The form names its algorithms: ML-DSA-65 operator keys and BLAKE2b-256 (a later algorithm gets a new prefix).
 *
 * A default-constructed RelayId is **none** (`empty()`): a field that names no relay. Every other value is 32 bytes.
 */
class RelayId {
public:
  RelayId() = default;

  /** The relay id of 32 raw bytes (a binary field: the card's signed bytes); nullopt for any other size. */
  static std::optional<RelayId> FromRaw(std::span<const uint8_t> raw);
  /**
   * The relay id `text` writes: `relay:` and exactly 43 base64url characters that decode to 32 bytes and encode back
   * to the same text (one spelling per id). Anything else (padding, spaces, another prefix, `Qm…`): nullopt.
   */
  static std::optional<RelayId> Parse(std::string_view text);
  /** The relay id of a founding operator set's encoding (`OperatorSet` in lib/net RelayCertificate.h): its hash. */
  static RelayId OfOperatorSet(std::span<const uint8_t> operator_set);

  /** None: no relay named. */
  bool empty() const { return !set_; }
  /** The 32 bytes (all zero when empty). */
  const std::array<uint8_t, kRelayIdBytes>& raw() const { return raw_; }
  ByteVector RawBytes() const { return ByteVector(raw_.begin(), raw_.end()); }
  /** `relay:…`; "" when empty. */
  std::string ToString() const;

  auto operator<=>(const RelayId&) const = default;
  bool operator==(const RelayId&) const = default;

private:
  std::array<uint8_t, kRelayIdBytes> raw_{};
  bool set_ = false;
};

/** `text` is a relay id's written form (RelayId::Parse succeeds). */
bool IsRelayIdForm(std::string_view text);

/** The bytes the relay id hashes: `"pp-browser:relay-id" 00 ‖ u8 1 ‖ operator_set` (tests and vectors print them). */
ByteVector RelayIdHashInput(std::span<const uint8_t> operator_set);

/**
 * A relay address (multi-relay MR008 as amended by SIGNED_FORMATS § 3.3): the base URL a client uses and the relay id
 * it must prove with its certificate. An account card's and a locator's relay entry, a relay of ours.
 */
struct RelayAddress {
  std::string origin;
  RelayId relay_id;

  bool operator==(const RelayAddress&) const = default;
};

} // namespace pbr
