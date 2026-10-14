#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

std::vector<uint8_t> BuildRegistrationSignBytes(const std::string& challenge, const std::string& public_key_b64,
                                                const std::string& kem_public_key_b64,
                                                const std::string& signature_alg, int64_t timestamp);

std::vector<uint8_t> BuildProfileUpdateSignBytes(const std::string& relay_user_id, const std::string& nickname,
                                                 int64_t timestamp);

/** `POST /v1/register/revoke` (#328): domain `pp-browser:relay-revoke-v1`, v1, relay_user_id, timestamp. */
std::vector<uint8_t> BuildAccountRevokeSignBytes(const std::string& relay_user_id, int64_t timestamp);

} // namespace pbr
