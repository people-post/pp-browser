#include "base/crypto/EncryptedPayload.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <sodium.h>

namespace pbr {

Roe<ByteVector> EncryptedPayload::EncodeBlob(const EncryptedBlob& blob) {
  if (blob.nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  ByteVector out;
  out.reserve(1 + blob.nonce.size() + blob.ciphertext.size());
  out.push_back(kEncryptedPayloadVersion);
  out.insert(out.end(), blob.nonce.begin(), blob.nonce.end());
  out.insert(out.end(), blob.ciphertext.begin(), blob.ciphertext.end());
  return out;
}

Roe<EncryptedBlob> EncryptedPayload::DecodeBlob(const ByteVector& bytes) {
  if (bytes.empty() || bytes[0] != kEncryptedPayloadVersion) {
    return Error("Unsupported encrypted payload version");
  }
  if (bytes.size() < 1 + kAeadNonceSize + crypto_aead_xchacha20poly1305_ietf_abytes()) {
    return Error("Encrypted payload too short");
  }
  EncryptedBlob blob;
  blob.nonce.assign(bytes.begin() + 1, bytes.begin() + 1 + kAeadNonceSize);
  blob.ciphertext.assign(bytes.begin() + 1 + kAeadNonceSize, bytes.end());
  return blob;
}

std::string EncryptedPayload::EncodeBase64(const ByteVector& blob) {
  return Base64Encode(blob);
}

Roe<ByteVector> EncryptedPayload::DecodeBase64(const std::string& payload_b64) {
  return Base64Decode(payload_b64);
}

} // namespace pbr
