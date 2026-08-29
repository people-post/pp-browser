#include "base/crypto/FileCipher.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/EncryptedPayload.h"
#include "base/crypto/MessageCipher.h"
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
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  const ByteVector aad_bytes(aad.begin(), aad.end());
  auto encrypted = MessageCipher::Encrypt(dek, plaintext, aad_bytes, *nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  return EncryptedPayload::EncodeBlob(*encrypted);
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

} // namespace pbr
