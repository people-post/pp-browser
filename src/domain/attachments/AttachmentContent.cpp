#include "domain/attachments/AttachmentContent.h"

#include "lib/content/PrivateContent.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlaintextMemoryCache.h"

#include <fstream>
#include <iterator>
#include "common/PbrCompat.h"

namespace pbr {

bool AttachmentBlobExists(const std::string& profile_dir, const std::vector<uint8_t>& content_hash) {
  return PrivateContentExists(profile_dir, content_hash);
}

Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, std::string_view profile_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename,
                                         const ByteVector& dek) {
  auto saved = SavePrivateContent(profile_dir, profile_id, content_hash, mime, plaintext, filename, dek);
  if (saved) {
    AttachmentPlaintextMemoryCache::Instance().Put(AttachmentHashHex(content_hash), plaintext);
  }
  return saved;
}

Roe<ByteVector> LoadAttachmentPlaintext(const std::string& profile_dir, std::string_view profile_id,
                                        const std::vector<uint8_t>& content_hash, const ByteVector& dek) {
  const std::string hash_hex = AttachmentHashHex(content_hash);
  ByteVector cached;
  if (AttachmentPlaintextMemoryCache::Instance().TryGet(hash_hex, cached)) {
    return cached;
  }
  auto plain = LoadPrivateContent(profile_dir, profile_id, content_hash, dek);
  if (plain) {
    AttachmentPlaintextMemoryCache::Instance().Put(hash_hex, *plain);
  }
  return plain;
}

Roe<std::string> EnsureAttachmentView(const std::string& profile_dir, std::string_view profile_id,
                                      const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                      const std::string& mime, const std::string& filename, const ByteVector& dek) {
  return EnsureAttachmentViewPath(profile_dir, thread_id, content_hash, mime, filename, [&]() {
    return LoadAttachmentPlaintext(profile_dir, profile_id, content_hash, dek);
  });
}

Roe<std::string> EnsureAttachmentPosterFromContent(const std::string& profile_dir, std::string_view profile_id,
                                                   const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                                   const std::string& mime, const std::string& filename,
                                                   const ByteVector& dek, const uint64_t known_byte_length) {
  return EnsureAttachmentPoster(
      profile_dir, thread_id, content_hash, mime, filename,
      [&]() { return LoadAttachmentPlaintext(profile_dir, profile_id, content_hash, dek); }, known_byte_length);
}

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, std::string_view profile_id,
                                      const std::string& thread_id, const ChatAttachmentFields& fields,
                                      const std::string& source_path, const ByteVector& dek) {
  std::ifstream input(source_path, std::ios::binary);
  if (!input) {
    return Error("Could not read sent attachment file");
  }
  const ByteVector plaintext((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (plaintext.empty()) {
    return Error("Sent attachment file is empty");
  }
  if (auto saved = SaveAttachmentPlaintext(profile_dir, profile_id, fields.content_hash, fields.mime, plaintext,
                                           fields.filename, dek);
      !saved) {
    return saved.error();
  }
  if (AttachmentAllowsInlinePrivateView(fields.mime, fields.byte_length > 0 ? fields.byte_length : plaintext.size())) {
    (void)EnsureAttachmentView(profile_dir, profile_id, thread_id, fields.content_hash, fields.mime, fields.filename,
                               dek);
  }
  return {};
}

} // namespace pbr
