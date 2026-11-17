#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

// What the relay's registry verifies (docs/contracts/SERVICE_ENDPOINTS.md § Signed relay commands): each command's
// bytes name the relay it is for, `audience` (its PeerId, multi-relay MR007), right after the sign version. Every
// builder answers empty bytes for an empty audience or a key that does not decode.

/** `POST /v1/register/finish`: `pp-browser:relay-register-v1`, v3. */
std::vector<uint8_t> BuildRegistrationSignBytes(const std::string& audience, const std::string& challenge,
                                                const std::string& public_key_b64,
                                                const std::string& kem_public_key_b64, const std::string& signature_alg,
                                                int64_t timestamp);

/** `POST /v1/profile/nickname`: `pp-browser:relay-profile-v1`, v2. */
std::vector<uint8_t> BuildProfileUpdateSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& nickname, int64_t timestamp);

/**
 * `POST /v1/register/revoke`: `pp-browser:relay-revoke-v1`, v2 — the registration it removes by the id
 * `register/finish` answered (MR007 item 5: a late revoke leaves a newer registration alone).
 */
std::vector<uint8_t> BuildAccountRevokeSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& registration_id, int64_t timestamp);

/** `POST /v1/mesh/heartbeat`: `pp-browser:relay-heartbeat-v1`, v2. */
std::vector<uint8_t> BuildRelayHeartbeatSignBytes(const std::string& audience, const std::string& account_id,
                                                  const std::string& peer_id, const std::string& state,
                                                  int64_t timestamp);

/** `POST /api/llm/v1/token/renew`: `pp-browser:relay-llm-token-v1`, v2 (the app does not call it yet). */
std::vector<uint8_t> BuildLlmTokenRenewSignBytes(const std::string& audience, const std::string& account_id,
                                                 int64_t timestamp);

} // namespace pbr
