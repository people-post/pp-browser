#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/MessageCipher.h"

#include "common/Error.h"

namespace pbr {

/** AEAD for attachment file bytes using a per-file content key (R004). */
class AttachmentContentCipher {
public:
  static Roe<ByteVector> GenerateContentKey();
  static Roe<EncryptedBlob> Encrypt(const ByteVector& content_key, const ByteVector& plaintext);
  /** Re-encrypt with a known nonce (peer blob heal uses envelope-stored nonce). */
  static Roe<EncryptedBlob> EncryptWithNonce(const ByteVector& content_key, const ByteVector& plaintext,
                                             const ByteVector& nonce);
  static Roe<ByteVector> Decrypt(const ByteVector& content_key, const ByteVector& nonce,
                                 const ByteVector& ciphertext, const ByteVector& content_hash);
};

} // namespace pbr
