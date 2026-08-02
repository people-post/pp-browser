#include "libp2p/integration/host/CallMediaFrameCrypto.h"

#include "base/crypto/MessageCipher.h"
#include "base/crypto/CryptoConstants.h"

#include <cstring>

namespace pbr {

namespace {

constexpr uint8_t kCallMediaAudioFrameVersion = 1;
constexpr size_t kCallMediaAudioHeaderBytes = 1 + 4 + 1; // ver + seq + mark

} // namespace

std::string BuildCallMediaFrameAad(const std::string& call_id, uint32_t media_epoch, uint32_t seq) {
  return "call-media|" + call_id + "|" + std::to_string(media_epoch) + "|" + std::to_string(seq);
}

Roe<std::vector<uint8_t>> EncryptCallMediaAudioFrame(const ByteVector& media_key, const std::string& call_id,
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
  std::vector<uint8_t> body(kCallMediaAudioHeaderBytes + encrypted->nonce.size() + encrypted->ciphertext.size());
  size_t i = 0;
  body[i++] = kCallMediaAudioFrameVersion;
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
  if (media_key.empty()) {
    return Error("call media key required");
  }
  if (body.size() < kCallMediaAudioHeaderBytes) {
    return Error("call media frame too short");
  }
  if (body[0] != kCallMediaAudioFrameVersion) {
    return Error("unsupported call media frame version");
  }
  const uint32_t seq = (static_cast<uint32_t>(body[1]) << 24) | (static_cast<uint32_t>(body[2]) << 16) |
                       (static_cast<uint32_t>(body[3]) << 8) | static_cast<uint32_t>(body[4]);
  const std::string aad_str = BuildCallMediaFrameAad(call_id, media_epoch, seq);
  const ByteVector aad(aad_str.begin(), aad_str.end());
  if (body.size() < kCallMediaAudioHeaderBytes + kAeadNonceSize) {
    return Error("call media frame truncated");
  }
  EncryptedBlob blob;
  blob.nonce.assign(body.begin() + kCallMediaAudioHeaderBytes,
                    body.begin() + kCallMediaAudioHeaderBytes + kAeadNonceSize);
  blob.ciphertext.assign(body.begin() + kCallMediaAudioHeaderBytes + kAeadNonceSize, body.end());
  auto decrypted = MessageCipher::Decrypt(media_key, blob, aad);
  if (!decrypted) {
    return decrypted.error();
  }
  return std::vector<uint8_t>(decrypted->begin(), decrypted->end());
}

} // namespace pbr
