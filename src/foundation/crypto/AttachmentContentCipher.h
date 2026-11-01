#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/MessageCipher.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

/** XChaCha20-Poly1305 adds its 16-byte tag to the plaintext; the nonce travels in the envelope, not the blob. */
inline constexpr uint64_t kAttachmentCipherOverheadBytes = 16;

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
  /** The same, consuming the ciphertext (a large file is not copied once more on the way in). */
  static Roe<ByteVector> Decrypt(const ByteVector& content_key, const ByteVector& nonce, ByteVector&& ciphertext,
                                 const ByteVector& content_hash);
};

} // namespace pbr
