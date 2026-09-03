#include "foundation/crypto/MessageCipher.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

Roe<ByteVector> MessageCipher::GenerateNonce() {
  EnsureSodiumInit();
  ByteVector nonce(kAeadNonceSize);
  randombytes_buf(nonce.data(), nonce.size());
  return nonce;
}

Roe<EncryptedBlob> MessageCipher::Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                                          const ByteVector& aad, const ByteVector& nonce) {
  if (session_key.size() != kSessionKeySize) {
    return Error("Invalid session key size");
  }
  if (nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  EnsureSodiumInit();

  EncryptedBlob blob;
  blob.nonce = nonce;
  blob.ciphertext.resize(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_abytes());
  unsigned long long ciphertext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(blob.ciphertext.data(), &ciphertext_len, plaintext.data(),
                                                 plaintext.size(), aad.data(), aad.size(), nullptr, nonce.data(),
                                                 session_key.data()) != 0) {
    return Error("AEAD encrypt failed");
  }
  blob.ciphertext.resize(static_cast<size_t>(ciphertext_len));
  return blob;
}

Roe<ByteVector> MessageCipher::Decrypt(const ByteVector& session_key, const EncryptedBlob& blob,
                                       const ByteVector& aad) {
  if (session_key.size() != kSessionKeySize) {
    return Error("Invalid session key size");
  }
  if (blob.nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  EnsureSodiumInit();

  ByteVector plaintext(blob.ciphertext.size());
  unsigned long long plaintext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext.data(), &plaintext_len, nullptr, blob.ciphertext.data(),
                                                 blob.ciphertext.size(), aad.data(), aad.size(), blob.nonce.data(),
                                                 session_key.data()) != 0) {
    return Error("AEAD decrypt failed");
  }
  plaintext.resize(static_cast<size_t>(plaintext_len));
  return plaintext;
}

} // namespace pbr
