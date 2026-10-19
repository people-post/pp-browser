#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "crypto/Aead.h"
#include "common/PbrCompat.h"

namespace pbr {

/** A nonce and the ciphertext (tag appended). */
using EncryptedBlob = pp::XChaCha20Poly1305::Sealed;

/** XChaCha20-Poly1305 for messages, blobs and keys — pp-cpp-crypto's, with pp-browser's key size. */
class MessageCipher {
public:
  static Roe<EncryptedBlob> Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                                    const ByteVector& aad, const ByteVector& nonce);
  static Roe<ByteVector> Decrypt(const ByteVector& session_key, const EncryptedBlob& blob, const ByteVector& aad);
  static Roe<ByteVector> GenerateNonce();
};

} // namespace pbr
