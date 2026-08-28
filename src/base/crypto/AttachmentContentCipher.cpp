#include "base/crypto/AttachmentContentCipher.h"

#include "base/crypto/AttachmentContentHash.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

Roe<ByteVector> AttachmentContentCipher::GenerateContentKey() {
  EnsureSodiumInit();
  ByteVector key(kSessionKeySize);
  randombytes_buf(key.data(), key.size());
  return key;
}

Roe<EncryptedBlob> AttachmentContentCipher::Encrypt(const ByteVector& content_key, const ByteVector& plaintext) {
  if (content_key.size() != kSessionKeySize) {
    return Error("Invalid attachment content key size");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  return MessageCipher::Encrypt(content_key, plaintext, hash.value(), nonce.value());
}

Roe<EncryptedBlob> AttachmentContentCipher::EncryptWithNonce(const ByteVector& content_key,
                                                             const ByteVector& plaintext,
                                                             const ByteVector& nonce) {
  if (content_key.size() != kSessionKeySize) {
    return Error("Invalid attachment content key size");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  return MessageCipher::Encrypt(content_key, plaintext, hash.value(), nonce);
}

Roe<ByteVector> AttachmentContentCipher::Decrypt(const ByteVector& content_key, const ByteVector& nonce,
                                                 const ByteVector& ciphertext, const ByteVector& content_hash) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash size");
  }
  if (content_key.size() != kSessionKeySize) {
    return Error("Invalid attachment content key size");
  }
  EncryptedBlob blob;
  blob.nonce = nonce;
  blob.ciphertext = ciphertext;
  auto plaintext = MessageCipher::Decrypt(content_key, blob, content_hash);
  if (!plaintext) {
    return plaintext.error();
  }
  auto hash = AttachmentContentHash(plaintext.value());
  if (!hash) {
    return hash.error();
  }
  if (hash.value() != content_hash) {
    return Error("Attachment content hash mismatch");
  }
  return plaintext;
}

} // namespace pbr
