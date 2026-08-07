#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

using ByteVector = std::vector<uint8_t>;

/** AEAD AAD for direct libp2p call-media audio frames (m1 / V026). */
std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq);

/**
 * AEAD AAD for SFU (`media_relay`) payloads (V032). Includes publisher stream_id so
 * frames cannot be replayed across streams.
 */
std::string BuildCallMediaSfuFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t stream_id,
                                      uint32_t seq);

/** Encrypt Opus payload → wire body for 1:1 call-media (after u64-BE length prefix). */
Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                      uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                      const std::vector<uint8_t>& opus_payload);

/** Decrypt 1:1 call-media wire body → Opus payload. */
Roe<std::vector<uint8_t>> DecryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                     uint32_t media_epoch, const std::vector<uint8_t>& body);

/** Encrypt Opus → opaque SFU payload (V032). Same framing as 1:1 body; distinct AAD. */
Roe<std::vector<uint8_t>> EncryptCallMediaSfuAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t stream_id, uint32_t seq,
                                                        uint8_t mark, const std::vector<uint8_t>& opus_payload);

/** Decrypt SFU payload → Opus. */
Roe<std::vector<uint8_t>> DecryptCallMediaSfuAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t stream_id,
                                                        const std::vector<uint8_t>& body);

} // namespace pbr
