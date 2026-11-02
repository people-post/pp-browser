#include "foundation/crypto/MessageCipher.h"

#include "foundation/crypto/CryptoConstants.h"
#include "common/PbrCompat.h"

namespace pbr {

static_assert(kAeadNonceSize == pp::XChaCha20Poly1305::kNonceBytes);
static_assert(kSessionKeySize == pp::XChaCha20Poly1305::kKeyBytes);

Roe<ByteVector> MessageCipher::GenerateNonce() {
  return pp::XChaCha20Poly1305::RandomNonce();
}

Roe<EncryptedBlob> MessageCipher::Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                                          const ByteVector& aad, const ByteVector& nonce) {
  return pp::XChaCha20Poly1305::Seal(session_key, plaintext, aad, nonce);
}

Roe<ByteVector> MessageCipher::Decrypt(const ByteVector& session_key, const EncryptedBlob& blob,
                                       const ByteVector& aad) {
  return pp::XChaCha20Poly1305::Open(session_key, blob, aad);
}

} // namespace pbr
