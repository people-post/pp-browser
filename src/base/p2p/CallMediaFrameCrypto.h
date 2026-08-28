#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

using ByteVector = std::vector<uint8_t>;

inline constexpr uint8_t kCallMediaChannelAudio = 0;
inline constexpr uint8_t kCallMediaChannelVideoLo = 1;
inline constexpr uint8_t kCallMediaFrameVersionV1 = 1;
inline constexpr uint8_t kCallMediaFrameVersionV2 = 2;

/** Decrypted call-media / SFU body (V034). */
struct CallMediaDecodedFrame {
  uint8_t channel = kCallMediaChannelAudio;
  uint32_t seq = 0;
  uint8_t mark = 0;
  std::vector<uint8_t> payload;
};

/** AEAD AAD for direct libp2p call-media frames (m1 / V026). v1 omits channel. */
std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq);
std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq,
                                   uint8_t channel);

/**
 * AEAD AAD for SFU (`media_relay`) payloads (V032). Includes publisher stream_id so
 * frames cannot be replayed across streams. v2 also binds channel.
 */
std::string BuildCallMediaSfuFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t stream_id,
                                      uint32_t seq);
std::string BuildCallMediaSfuFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t stream_id,
                                      uint32_t seq, uint8_t channel);

/** Encrypt payload → v2 wire body (channel 0 = Opus, 1 = H264 AU). */
Roe<std::vector<uint8_t>> EncryptCallMediaFrame(const ByteVector& media_key, const std::string& call_id,
                                                uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                uint8_t channel, const std::vector<uint8_t>& payload);

/** Decrypt 1:1 call-media wire body (v1 → channel 0; v2 → channel byte). */
Roe<CallMediaDecodedFrame> DecryptCallMediaFrame(const ByteVector& media_key, const std::string& call_id,
                                                 uint32_t media_epoch, const std::vector<uint8_t>& body);

/** Encrypt Opus payload → wire body for 1:1 call-media (v2 channel 0). */
Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                      uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                      const std::vector<uint8_t>& opus_payload);

/** Decrypt 1:1 call-media wire body → Opus payload (rejects non-audio channel). */
Roe<std::vector<uint8_t>> DecryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                     uint32_t media_epoch, const std::vector<uint8_t>& body);

/** Compat/test: emit a v1 audio body (no channel byte). Production send is v2. */
Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrameV1(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                        const std::vector<uint8_t>& opus_payload);

/**
 * Encrypt opaque SFU payload (V032/V034). Distinct AAD; v2 includes channel.
 * One seal per AU under the shared call media key (V004) — not per subscriber.
 */
Roe<std::vector<uint8_t>> EncryptCallMediaSfuFrame(const ByteVector& media_key, const std::string& call_id,
                                                   uint32_t media_epoch, uint32_t stream_id, uint32_t seq,
                                                   uint8_t mark, uint8_t channel,
                                                   const std::vector<uint8_t>& payload);

/** Decrypt SFU payload. `channel` is the N021 channel_id used for v2 AAD. */
Roe<CallMediaDecodedFrame> DecryptCallMediaSfuFrame(const ByteVector& media_key, const std::string& call_id,
                                                    uint32_t media_epoch, uint32_t stream_id, uint8_t channel,
                                                    const std::vector<uint8_t>& body);

/** Encrypt Opus → opaque SFU payload (v2 channel 0). */
Roe<std::vector<uint8_t>> EncryptCallMediaSfuAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t stream_id, uint32_t seq,
                                                        uint8_t mark, const std::vector<uint8_t>& opus_payload);

/** Decrypt SFU payload → Opus (channel 0 AAD). */
Roe<std::vector<uint8_t>> DecryptCallMediaSfuAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t stream_id,
                                                        const std::vector<uint8_t>& body);

} // namespace pbr
