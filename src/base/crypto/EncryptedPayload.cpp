#include "base/crypto/EncryptedPayload.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <algorithm>
#include <sodium.h>

namespace pbr {

Roe<ByteVector> EncryptedPayload::EncodeBlob(const EncryptedBlob& blob) {
  if (blob.nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  // Size the vector once (no reserve/push_back/realloc). GCC 14 -O3 falsely
  // reports -Wfree-nonheap-object on the previous reserve + push_back path.
  ByteVector out(1 + blob.nonce.size() + blob.ciphertext.size());
  out[0] = kEncryptedPayloadVersion;
  std::copy(blob.nonce.begin(), blob.nonce.end(), out.begin() + 1);
  std::copy(blob.ciphertext.begin(), blob.ciphertext.end(),
            out.begin() + 1 + static_cast<std::ptrdiff_t>(blob.nonce.size()));
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
