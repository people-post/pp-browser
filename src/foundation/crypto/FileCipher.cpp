#include "foundation/crypto/FileCipher.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/crypto/MessageCipher.h"
#include <sodium.h>
#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

std::string FileCipher::BuildAad(std::string_view purpose, std::string_view profile_id,
                                 const uint8_t schema_version) {
  return std::string(purpose) + "|" + std::string(profile_id) + "|" + std::to_string(schema_version);
}

Roe<ByteVector> FileCipher::Encrypt(const ByteVector& dek, const ByteVector& plaintext, std::string_view aad) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  EnsureSodiumInit();
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  if (nonce->size() != kAeadNonceSize || dek.size() != crypto_aead_xchacha20poly1305_ietf_keybytes()) {
    return Error("Invalid key or nonce size");
  }
  // Sealed straight into the output (version, nonce, ciphertext + tag): one buffer, however large the file.
  // Same bytes as EncryptedPayload::EncodeBlob over a MessageCipher::Encrypt result.
  ByteVector out(1 + kAeadNonceSize + plaintext.size() + crypto_aead_xchacha20poly1305_ietf_abytes());
  out[0] = kEncryptedPayloadVersion;
  std::copy(nonce->begin(), nonce->end(), out.begin() + 1);
  unsigned long long length = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          out.data() + 1 + kAeadNonceSize, &length, plaintext.data(), plaintext.size(),
          reinterpret_cast<const unsigned char*>(aad.data()), aad.size(), nullptr, nonce->data(), dek.data()) != 0) {
    return Error("AEAD encrypt failed");
  }
  out.resize(1 + kAeadNonceSize + static_cast<size_t>(length));
  return out;
}

Roe<ByteVector> FileCipher::Decrypt(const ByteVector& dek, const ByteVector& blob, std::string_view aad) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  auto decoded = EncryptedPayload::DecodeBlob(blob);
  if (!decoded) {
    return decoded.error();
  }
  const ByteVector aad_bytes(aad.begin(), aad.end());
  return MessageCipher::Decrypt(dek, *decoded, aad_bytes);
}

Roe<ByteVector> FileCipher::Decrypt(const ByteVector& dek, ByteVector&& blob, std::string_view aad) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  auto decoded = EncryptedPayload::DecodeBlob(std::move(blob));
  if (!decoded) {
    return decoded.error();
  }
  const ByteVector aad_bytes(aad.begin(), aad.end());
  return MessageCipher::Decrypt(dek, *decoded, aad_bytes);
}

} // namespace pbr
