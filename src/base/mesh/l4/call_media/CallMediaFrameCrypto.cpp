#include "base/mesh/l4/call_media/CallMediaFrameCrypto.h"

#include "base/crypto/MessageCipher.h"
#include "base/crypto/CryptoConstants.h"

#include <cstring>
#include "common/PbrCompat.h"

namespace pbr {

constexpr size_t kCallMediaV1HeaderBytes = 1 + 4 + 1;     // ver + seq + mark
constexpr size_t kCallMediaV2HeaderBytes = 1 + 4 + 1 + 1; // + channel

namespace {

uint32_t ReadSeq(const std::vector<uint8_t>& body) {
  return (static_cast<uint32_t>(body[1]) << 24) | (static_cast<uint32_t>(body[2]) << 16) |
         (static_cast<uint32_t>(body[3]) << 8) | static_cast<uint32_t>(body[4]);
}

Roe<std::vector<uint8_t>> EncryptBody(const ByteVector& media_key, const std::string& aad_str, uint32_t seq,
                                      uint8_t mark, uint8_t channel, const std::vector<uint8_t>& payload) {
  if (media_key.empty()) {
    return Error("call media key required");
  }
  const ByteVector aad(aad_str.begin(), aad_str.end());
  const ByteVector plain(payload.begin(), payload.end());
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  auto encrypted = MessageCipher::Encrypt(media_key, plain, aad, *nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  std::vector<uint8_t> body(kCallMediaV2HeaderBytes + encrypted->nonce.size() + encrypted->ciphertext.size());
  size_t i = 0;
  body[i++] = kCallMediaFrameVersionV2;
  body[i++] = static_cast<uint8_t>((seq >> 24) & 0xff);
  body[i++] = static_cast<uint8_t>((seq >> 16) & 0xff);
  body[i++] = static_cast<uint8_t>((seq >> 8) & 0xff);
  body[i++] = static_cast<uint8_t>(seq & 0xff);
  body[i++] = mark;
  body[i++] = channel;
  std::memcpy(body.data() + i, encrypted->nonce.data(), encrypted->nonce.size());
  i += encrypted->nonce.size();
  std::memcpy(body.data() + i, encrypted->ciphertext.data(), encrypted->ciphertext.size());
  return body;
}

Roe<CallMediaDecodedFrame> DecryptBody(const ByteVector& media_key, const std::string& aad_str,
                                       uint8_t channel, uint32_t seq, uint8_t mark, size_t header_bytes,
                                       const std::vector<uint8_t>& body) {
  if (media_key.empty()) {
    return Error("call media key required");
  }
  if (body.size() < header_bytes + kAeadNonceSize) {
    return Error("call media frame truncated");
  }
  const ByteVector aad(aad_str.begin(), aad_str.end());
  EncryptedBlob blob;
  blob.nonce.assign(body.begin() + static_cast<std::ptrdiff_t>(header_bytes),
                    body.begin() + static_cast<std::ptrdiff_t>(header_bytes + kAeadNonceSize));
  blob.ciphertext.assign(body.begin() + static_cast<std::ptrdiff_t>(header_bytes + kAeadNonceSize), body.end());
  auto decrypted = MessageCipher::Decrypt(media_key, blob, aad);
  if (!decrypted) {
    return decrypted.error();
  }
  CallMediaDecodedFrame out;
  out.channel = channel;
  out.seq = seq;
  out.mark = mark;
  out.payload.assign(decrypted->begin(), decrypted->end());
  return out;
}

Roe<CallMediaDecodedFrame> DecryptVersioned(const ByteVector& media_key, const std::vector<uint8_t>& body,
                                            const std::string& aad_v1, const std::string& aad_v2) {
  if (body.size() < kCallMediaV1HeaderBytes) {
    return Error("call media frame too short");
  }
  const uint8_t ver = body[0];
  const uint32_t seq = ReadSeq(body);
  const uint8_t mark = body[5];
  if (ver == kCallMediaFrameVersionV1) {
    return DecryptBody(media_key, aad_v1, kCallMediaChannelAudio, seq, mark, kCallMediaV1HeaderBytes, body);
  }
  if (ver == kCallMediaFrameVersionV2) {
    if (body.size() < kCallMediaV2HeaderBytes) {
      return Error("call media frame too short");
    }
    return DecryptBody(media_key, aad_v2, body[6], seq, mark, kCallMediaV2HeaderBytes, body);
  }
  return Error("unsupported call media frame version");
}

} // namespace

std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq) {
  return "call-media|" + call_id + "|" + std::to_string(media_epoch) + "|" + std::to_string(seq);
}

std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq,
                                   uint8_t channel) {
  return BuildCallMediaFrameAad(call_id, media_epoch, seq) + "|" + std::to_string(channel);
}

std::string BuildCallMediaSfuFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t stream_id,
                                      uint32_t seq) {
  return "call-media-sfu|" + call_id + "|" + std::to_string(media_epoch) + "|" + std::to_string(stream_id) +
         "|" + std::to_string(seq);
}

std::string BuildCallMediaSfuFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t stream_id,
                                      uint32_t seq, uint8_t channel) {
  return BuildCallMediaSfuFrameAad(call_id, media_epoch, stream_id, seq) + "|" + std::to_string(channel);
}

Roe<std::vector<uint8_t>> EncryptCallMediaFrame(const ByteVector& media_key, const std::string& call_id,
                                                uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                uint8_t channel, const std::vector<uint8_t>& payload) {
  return EncryptBody(media_key, BuildCallMediaFrameAad(call_id, media_epoch, seq, channel), seq, mark, channel,
                     payload);
}

Roe<CallMediaDecodedFrame> DecryptCallMediaFrame(const ByteVector& media_key, const std::string& call_id,
                                                 uint32_t media_epoch, const std::vector<uint8_t>& body) {
  if (body.size() < kCallMediaV1HeaderBytes) {
    return Error("call media frame too short");
  }
  const uint32_t seq = ReadSeq(body);
  const uint8_t channel = (body[0] == kCallMediaFrameVersionV2 && body.size() >= kCallMediaV2HeaderBytes)
                              ? body[6]
                              : kCallMediaChannelAudio;
  return DecryptVersioned(media_key, body, BuildCallMediaFrameAad(call_id, media_epoch, seq),
                          BuildCallMediaFrameAad(call_id, media_epoch, seq, channel));
}

Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                      uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                      const std::vector<uint8_t>& opus_payload) {
  return EncryptCallMediaFrame(media_key, call_id, media_epoch, seq, mark, kCallMediaChannelAudio,
                               opus_payload);
}

Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrameV1(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t seq, uint8_t mark,
                                                        const std::vector<uint8_t>& opus_payload) {
  if (media_key.empty()) {
    return Error("call media key required");
  }
  const std::string aad_str = BuildCallMediaFrameAad(call_id, media_epoch, seq);
  const ByteVector aad(aad_str.begin(), aad_str.end());
  const ByteVector plain(opus_payload.begin(), opus_payload.end());
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  auto encrypted = MessageCipher::Encrypt(media_key, plain, aad, *nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  std::vector<uint8_t> body(kCallMediaV1HeaderBytes + encrypted->nonce.size() + encrypted->ciphertext.size());
  size_t i = 0;
  body[i++] = kCallMediaFrameVersionV1;
  body[i++] = static_cast<uint8_t>((seq >> 24) & 0xff);
  body[i++] = static_cast<uint8_t>((seq >> 16) & 0xff);
  body[i++] = static_cast<uint8_t>((seq >> 8) & 0xff);
  body[i++] = static_cast<uint8_t>(seq & 0xff);
  body[i++] = mark;
  std::memcpy(body.data() + i, encrypted->nonce.data(), encrypted->nonce.size());
  i += encrypted->nonce.size();
  std::memcpy(body.data() + i, encrypted->ciphertext.data(), encrypted->ciphertext.size());
  return body;
}

Roe<std::vector<uint8_t>> DecryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                     uint32_t media_epoch, const std::vector<uint8_t>& body) {
  auto decoded = DecryptCallMediaFrame(media_key, call_id, media_epoch, body);
  if (!decoded) {
    return decoded.error();
  }
  if (decoded->channel != kCallMediaChannelAudio) {
    return Error("call media frame is not audio");
  }
  return std::move(decoded->payload);
}

Roe<std::vector<uint8_t>> EncryptCallMediaSfuFrame(const ByteVector& media_key, const std::string& call_id,
                                                   uint32_t media_epoch, uint32_t stream_id, uint32_t seq,
                                                   uint8_t mark, uint8_t channel,
                                                   const std::vector<uint8_t>& payload) {
  return EncryptBody(media_key, BuildCallMediaSfuFrameAad(call_id, media_epoch, stream_id, seq, channel), seq,
                     mark, channel, payload);
}

Roe<CallMediaDecodedFrame> DecryptCallMediaSfuFrame(const ByteVector& media_key, const std::string& call_id,
                                                    uint32_t media_epoch, uint32_t stream_id, uint8_t channel,
                                                    const std::vector<uint8_t>& body) {
  if (body.size() < kCallMediaV1HeaderBytes) {
    return Error("call media frame too short");
  }
  const uint32_t seq = ReadSeq(body);
  auto decoded = DecryptVersioned(
      media_key, body, BuildCallMediaSfuFrameAad(call_id, media_epoch, stream_id, seq),
      BuildCallMediaSfuFrameAad(call_id, media_epoch, stream_id, seq, channel));
  if (!decoded) {
    return decoded.error();
  }
  if (body[0] == kCallMediaFrameVersionV2 && decoded->channel != channel) {
    return Error("call media SFU channel mismatch");
  }
  return decoded;
}

Roe<std::vector<uint8_t>> EncryptCallMediaSfuAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t stream_id, uint32_t seq,
                                                        uint8_t mark, const std::vector<uint8_t>& opus_payload) {
  return EncryptCallMediaSfuFrame(media_key, call_id, media_epoch, stream_id, seq, mark, kCallMediaChannelAudio,
                                  opus_payload);
}

Roe<std::vector<uint8_t>> DecryptCallMediaSfuAudioFrame(const ByteVector& media_key, const std::string& call_id,
                                                        uint32_t media_epoch, uint32_t stream_id,
                                                        const std::vector<uint8_t>& body) {
  auto decoded = DecryptCallMediaSfuFrame(media_key, call_id, media_epoch, stream_id, kCallMediaChannelAudio, body);
  if (!decoded) {
    return decoded.error();
  }
  if (decoded->channel != kCallMediaChannelAudio) {
    return Error("call media SFU frame is not audio");
  }
  return std::move(decoded->payload);
}

} // namespace pbr
