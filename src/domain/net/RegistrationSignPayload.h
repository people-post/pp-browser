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

} // namespace pbr
