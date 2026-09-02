#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/MessageCipher.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class EncryptedPayload {
public:
  static Roe<ByteVector> EncodeBlob(const EncryptedBlob& blob);
  static Roe<EncryptedBlob> DecodeBlob(const ByteVector& bytes);
  static std::string EncodeBase64(const ByteVector& blob);
  static Roe<ByteVector> DecodeBase64(const std::string& payload_b64);
};

} // namespace pbr
