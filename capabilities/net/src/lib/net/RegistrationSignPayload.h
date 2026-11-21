#pragma once

#include "common/ExtArea.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

// What the relay's registry verifies (release-compatibility SIGNED_FORMATS § 2.8; docs/contracts/SERVICE_ENDPOINTS.md
// § The audience): `domain ‖ 0x00 ‖ u8 4 ‖ LenUtf8(audience) ‖ fields ‖ ext area`, the audience the PeerId of the relay
// the call is for. Every builder answers empty bytes for an empty audience.

/**
 * `POST /v1/register/finish` (`pp-browser:relay-register`): the challenge, the account's typed signing key and typed
 * KEM key (standard base64 of the typed blobs the body carries as `public_key` and `kem_public_key_b64`), the
 * timestamp. Empty, too, for a key that is not a typed blob of its kind this build writes.
 *
 *   "pp-browser:relay-register" 00 ‖ u8 4 ‖ LenUtf8 audience ‖ LenUtf8 challenge
 *   ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed) ‖ i64 timestamp ‖ ext area
 */
std::vector<uint8_t> BuildRegistrationSignBytes(const std::string& audience, const std::string& challenge,
                                                const std::string& typed_public_key_b64,
                                                const std::string& typed_kem_public_key_b64, int64_t timestamp,
                                                const pp::ExtArea& ext = {});

/** `POST /v1/profile/nickname` (`pp-browser:relay-profile`): `account_id`, `nickname`, `timestamp`. */
std::vector<uint8_t> BuildProfileUpdateSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& nickname, int64_t timestamp,
                                                 const pp::ExtArea& ext = {});

/**
 * `POST /v1/register/revoke` (`pp-browser:relay-revoke`): `account_id`, `registration_id`, `timestamp` — the
 * registration it removes by the id `register/finish` answered (MR007 item 5: a late revoke leaves a newer
 * registration alone).
 */
std::vector<uint8_t> BuildAccountRevokeSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& registration_id, int64_t timestamp,
                                                 const pp::ExtArea& ext = {});

/** `POST /v1/mesh/heartbeat` (`pp-browser:relay-heartbeat`, pp-node): `account_id`, `peer_id`, `state`, `timestamp`. */
std::vector<uint8_t> BuildRelayHeartbeatSignBytes(const std::string& audience, const std::string& account_id,
                                                  const std::string& peer_id, const std::string& state,
                                                  int64_t timestamp, const pp::ExtArea& ext = {});

/**
 * `POST /api/llm/v1/token/renew` (`pp-browser:relay-llm-token`): `account_id`, `timestamp`
 * (`RenewBriefLlmTokenWithIdentity`).
 */
std::vector<uint8_t> BuildLlmTokenRenewSignBytes(const std::string& audience, const std::string& account_id,
                                                 int64_t timestamp, const pp::ExtArea& ext = {});

} // namespace pbr
