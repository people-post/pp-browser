#include "foundation/crypto/MediaKeyWrap.h"

#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/crypto/MessageCipher.h"

#include "common/PbrCompat.h"

namespace pbr {

std::string MediaKeyWrapAad(const std::string_view label, const std::string& session_id, const uint32_t epoch,
                            const std::string& key_id) {
  return std::string(label) + "|" + session_id + "|" + std::to_string(epoch) + "|" + key_id;
}

Roe<std::string> WrapMediaKeyB64(const std::string_view label, const ByteVector& session_key,
                                 const ByteVector& key_bytes, const std::string& session_id, const uint32_t epoch,
                                 const std::string& key_id) {
  if (label.empty() || session_key.empty() || key_bytes.empty() || session_id.empty() || key_id.empty()) {
    return Error("Wrapping a media key requires a label, session key, key bytes, session id and key id");
  }
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  const std::string aad = MediaKeyWrapAad(label, session_id, epoch, key_id);
  auto encrypted = MessageCipher::Encrypt(session_key, key_bytes, ByteVector(aad.begin(), aad.end()), *nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  auto blob = EncryptedPayload::EncodeBlob(*encrypted);
  if (!blob) {
    return blob.error();
  }
  return EncryptedPayload::EncodeBase64(*blob);
}

Roe<ByteVector> UnwrapMediaKeyB64(const std::string_view label, const ByteVector& session_key,
                                  const std::string& wrapped_key_b64, const std::string& session_id,
                                  const uint32_t epoch, const std::string& key_id) {
  if (label.empty() || session_key.empty() || wrapped_key_b64.empty() || session_id.empty() || key_id.empty()) {
    return Error("Unwrapping a media key requires a label, session key, wrapped key, session id and key id");
  }
  auto blob_bytes = EncryptedPayload::DecodeBase64(wrapped_key_b64);
  if (!blob_bytes) {
    return blob_bytes.error();
  }
  auto blob = EncryptedPayload::DecodeBlob(*blob_bytes);
  if (!blob) {
    return blob.error();
  }
  const std::string aad = MediaKeyWrapAad(label, session_id, epoch, key_id);
  return MessageCipher::Decrypt(session_key, *blob, ByteVector(aad.begin(), aad.end()));
}

} // namespace pbr
