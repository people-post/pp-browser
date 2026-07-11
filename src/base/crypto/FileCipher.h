#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <string>
#include <string_view>

namespace pbr {

/** AEAD for at-rest blobs under a profile DEK (AAD binds purpose + profile). */
class FileCipher {
public:
  static std::string BuildAad(std::string_view purpose, std::string_view profile_id, uint8_t schema_version = 1);
  static Roe<ByteVector> Encrypt(const ByteVector& dek, const ByteVector& plaintext, std::string_view aad);
  static Roe<ByteVector> Decrypt(const ByteVector& dek, const ByteVector& blob, std::string_view aad);
};

} // namespace pbr
