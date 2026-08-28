#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

struct EncryptedBlob {
  ByteVector nonce;
  ByteVector ciphertext;
};

class MessageCipher {
public:
  static Roe<EncryptedBlob> Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                                    const ByteVector& aad, const ByteVector& nonce);
  static Roe<ByteVector> Decrypt(const ByteVector& session_key, const EncryptedBlob& blob, const ByteVector& aad);
  static Roe<ByteVector> GenerateNonce();
};

} // namespace pbr
