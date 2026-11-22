#pragma once

#include "foundation/crypto/RelayId.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a relay says of itself (`GET /v1/relay-identity` v2; release-compatibility SIGNED_FORMATS § 3.4): its relay id
 * and its certificate. Neither is taken on its word: the certificate proves the relay id and the transport PeerId
 * (RelayCertificate.h, RelayPins), so the answer itself needs no signature.
 */
struct RelayIdentity {
  RelayId relay_id;
  /** Standard base64 of the certificate blob (RelayCertificate.h); not checked here. */
  std::string certificate_b64;

  bool operator==(const RelayIdentity&) const = default;
};

/** Codes an identity answer is refused with (the Error's message starts with one). */
inline constexpr const char kRelayIdentityUnavailable[] = "relay_identity_unavailable";
inline constexpr const char kRelayIdentityInvalid[] = "relay_identity_invalid";

/** The answer's `schema_version`: 2 from the cutover (1 named a PeerId and a key, and is not read). */
inline constexpr int64_t kRelayIdentitySchemaVersion = 2;

/**
 * The body of `GET /v1/relay-identity` (docs/contracts/SERVICE_ENDPOINTS.md § Relay identity):
 * `{ "schema_version": 2, "relay_id": "relay:…", "certificate_b64": "…" }`, taken when `schema_version` is 2 or
 * higher (T15: a newer one is read for these fields), `relay_id` is a relay id's written form and `certificate_b64` a
 * non-empty string. Unknown keys are ignored. Anything else (version 1 or none: an older relay) is
 * kRelayIdentityInvalid (Err::Network::RelayIdentityUnavailable, as no identity). The certificate is checked by
 * whoever takes it (RelayPins: against the relay id it expects, and that it lists the origin asked).
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
