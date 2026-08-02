#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

using ByteVector = std::vector<uint8_t>;

/** AEAD AAD for direct libp2p call-media audio frames (m1 / V026). */
std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq);

/** Encrypt Opus payload → length-prefixed wire body (after u64-BE length prefix). */
Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                      uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                      const std::vector<uint8_t>& opus_payload);

/** Decrypt wire body → Opus payload. */
Roe<std::vector<uint8_t>> DecryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                     uint32_t media_epoch, const std::vector<uint8_t>& body);

} // namespace pbr
