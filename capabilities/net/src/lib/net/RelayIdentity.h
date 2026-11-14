#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** A relay's identity (multi-relay MR007): its ML-DSA-65 key and the PeerId derived from it, as a pp-node's. */
struct RelayIdentity {
  std::string peer_id;
  /** Standard (padded) base64 of the raw 1952-byte key. */
  std::string public_key_b64;

  bool operator==(const RelayIdentity&) const = default;
};

/** Codes an identity answer is refused with (the Error's message starts with one). */
inline constexpr const char kRelayIdentityUnavailable[] = "relay_identity_unavailable";
inline constexpr const char kRelayIdentityInvalid[] = "relay_identity_invalid";

/**
 * The body of `GET /v1/relay-identity` (docs/contracts/SERVICE_ENDPOINTS.md § Relay identity), taken only when
 * `schema_version` is 1, `signature_alg` is `ml-dsa-65`, `public_key_b64` is canonical base64 of 1952 bytes, and the
 * key derives `peer_id` (pp::PeerIdFromMlDsaPublicKey). Unknown keys are ignored. Anything else:
 * kRelayIdentityInvalid (Err::Network::RelayIdentityUnavailable, as no identity). Unsigned on purpose (MR007).
 */
Roe<RelayIdentity> ParseRelayIdentity(const std::string& body);

/**
 * Asks the relay at `base_url` (`<origin>/api/relay`) for its identity, unauthenticated. Unreachable, a `503
 * relay_identity_unavailable` or any other status, and an answer ParseRelayIdentity refuses are all
 * Err::Network::RelayIdentityUnavailable: no identity, nothing pinned. Blocks (HTTP, bounded).
 * `public_only`: a relay that is not ours (a QR locator's): https to a public address only, as its client
 * (HttpRelayClient's `public_only`).
 */
Roe<RelayIdentity> FetchRelayIdentity(const std::string& base_url, bool public_only = false);

} // namespace pbr
