#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/MessageCipher.h"

#include "common/Error.h"

#include <string>

namespace pbr {

class EncryptedPayload {
public:
  static Roe<ByteVector> EncodeBlob(const EncryptedBlob& blob);
  static Roe<EncryptedBlob> DecodeBlob(const ByteVector& bytes);
  static std::string EncodeBase64(const ByteVector& blob);
  static Roe<ByteVector> DecodeBase64(const std::string& payload_b64);
};

} // namespace pbr
