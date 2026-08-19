#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** AEAD helpers for transcript rows under the profile DEK. */
class TranscriptCipher {
public:
  static std::string BuildMessageAad(std::string_view profile_id, std::string_view thread_id,
                                     std::string_view message_id);
  static std::string BuildPreviewAad(std::string_view profile_id, std::string_view thread_id);
  static std::string BuildMemoryAad(std::string_view profile_id, std::string_view thread_id, std::string_view key);

  static Roe<ByteVector> EncryptMessageBody(const ByteVector& dek, std::string_view profile_id,
                                            std::string_view thread_id, std::string_view message_id,
                                            const ByteVector& plaintext);
  static Roe<ByteVector> DecryptMessageBody(const ByteVector& dek, std::string_view profile_id,
                                            std::string_view thread_id, std::string_view message_id,
                                            const ByteVector& ciphertext);

  static Roe<ByteVector> EncryptPreview(const ByteVector& dek, std::string_view profile_id,
                                       std::string_view thread_id, std::string_view preview_text);
  static Roe<std::string> DecryptPreview(const ByteVector& dek, std::string_view profile_id,
                                         std::string_view thread_id, const ByteVector& ciphertext);

  static Roe<ByteVector> EncryptMemoryValue(const ByteVector& dek, std::string_view profile_id,
                                            std::string_view thread_id, std::string_view key,
                                            std::string_view value_text);
  static Roe<std::string> DecryptMemoryValue(const ByteVector& dek, std::string_view profile_id,
                                             std::string_view thread_id, std::string_view key,
                                             const ByteVector& ciphertext);
};

} // namespace pbr
