#include "base/messaging/TranscriptCipher.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/FileCipher.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string BuildAad(std::string_view purpose, std::string_view profile_id, std::string_view suffix) {
  return std::string(purpose) + "|" + std::string(profile_id) + "|" + std::string(suffix) + "|1";
}

Roe<void> RequireDek(const ByteVector& dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  return {};
}

} // namespace

std::string TranscriptCipher::BuildMessageAad(std::string_view profile_id, std::string_view thread_id,
                                              std::string_view message_id) {
  return BuildAad("transcript-body", profile_id, thread_id) + "|" + std::string(message_id);
}

std::string TranscriptCipher::BuildPreviewAad(std::string_view profile_id, std::string_view thread_id) {
  return BuildAad("transcript-preview", profile_id, thread_id);
}

std::string TranscriptCipher::BuildMemoryAad(std::string_view profile_id, std::string_view thread_id,
                                             std::string_view key) {
  return BuildAad("transcript-memory", profile_id, thread_id) + "|" + std::string(key);
}

Roe<ByteVector> TranscriptCipher::EncryptMessageBody(const ByteVector& dek, std::string_view profile_id,
                                                   std::string_view thread_id, std::string_view message_id,
                                                   const ByteVector& plaintext) {
  if (auto check = RequireDek(dek); !check) {
    return check.error();
  }
  return FileCipher::Encrypt(dek, plaintext, BuildMessageAad(profile_id, thread_id, message_id));
}

Roe<ByteVector> TranscriptCipher::DecryptMessageBody(const ByteVector& dek, std::string_view profile_id,
                                                   std::string_view thread_id, std::string_view message_id,
                                                   const ByteVector& ciphertext) {
  if (auto check = RequireDek(dek); !check) {
    return check.error();
  }
  return FileCipher::Decrypt(dek, ciphertext, BuildMessageAad(profile_id, thread_id, message_id));
}

Roe<ByteVector> TranscriptCipher::EncryptPreview(const ByteVector& dek, std::string_view profile_id,
                                                std::string_view thread_id, std::string_view preview_text) {
  if (auto check = RequireDek(dek); !check) {
    return check.error();
  }
  const ByteVector plain(preview_text.begin(), preview_text.end());
  return FileCipher::Encrypt(dek, plain, BuildPreviewAad(profile_id, thread_id));
}

Roe<std::string> TranscriptCipher::DecryptPreview(const ByteVector& dek, std::string_view profile_id,
                                                 std::string_view thread_id, const ByteVector& ciphertext) {
  if (auto check = RequireDek(dek); !check) {
    return check.error();
  }
  auto plain = FileCipher::Decrypt(dek, ciphertext, BuildPreviewAad(profile_id, thread_id));
  if (!plain) {
    return plain.error();
  }
  return std::string(plain->begin(), plain->end());
}

Roe<ByteVector> TranscriptCipher::EncryptMemoryValue(const ByteVector& dek, std::string_view profile_id,
                                                    std::string_view thread_id, std::string_view key,
                                                    std::string_view value_text) {
  if (auto check = RequireDek(dek); !check) {
    return check.error();
  }
  const ByteVector plain(value_text.begin(), value_text.end());
  return FileCipher::Encrypt(dek, plain, BuildMemoryAad(profile_id, thread_id, key));
}

Roe<std::string> TranscriptCipher::DecryptMemoryValue(const ByteVector& dek, std::string_view profile_id,
                                                     std::string_view thread_id, std::string_view key,
                                                     const ByteVector& ciphertext) {
  if (auto check = RequireDek(dek); !check) {
    return check.error();
  }
  auto plain = FileCipher::Decrypt(dek, ciphertext, BuildMemoryAad(profile_id, thread_id, key));
  if (!plain) {
    return plain.error();
  }
  return std::string(plain->begin(), plain->end());
}

} // namespace pbr
