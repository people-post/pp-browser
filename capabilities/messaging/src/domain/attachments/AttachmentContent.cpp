#include "domain/attachments/AttachmentContent.h"
#include "common/chat/MessagingLimits.h"

#include "common/chat/MessagingLimits.h"
#include "lib/content/PrivateContent.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlaintextMemoryCache.h"

#include "common/PbrCompat.h"

namespace pbr {

bool AttachmentBlobExists(const CasStore& content, const std::vector<uint8_t>& content_hash) {
  return PrivateContentExists(content, content_hash);
}

Roe<std::string> SaveAttachmentPlaintext(CasStore& content, const std::vector<uint8_t>& content_hash,
                                         const std::string& mime, const ByteVector& plaintext,
                                         const std::string& filename, const ByteVector& dek) {
  auto saved = SavePrivateContent(content, content_hash, mime, plaintext, filename, dek);
  if (saved) {
    AttachmentPlaintextMemoryCache::Instance().Put(AttachmentHashHex(content_hash), plaintext);
  }
  return saved;
}

Roe<ByteVector> LoadAttachmentPlaintext(const CasStore& content, const std::vector<uint8_t>& content_hash,
                                        const ByteVector& dek) {
  const std::string hash_hex = AttachmentHashHex(content_hash);
  ByteVector cached;
  if (AttachmentPlaintextMemoryCache::Instance().TryGet(hash_hex, cached)) {
    return cached;
  }
  auto plain = LoadPrivateContent(content, content_hash, dek);
  if (plain) {
    AttachmentPlaintextMemoryCache::Instance().Put(hash_hex, *plain);
  }
  return plain;
}

Roe<std::string> EnsureAttachmentView(const std::string& profile_dir, const CasStore& content,
                                      const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                      const std::string& mime, const std::string& filename, const ByteVector& dek) {
  return EnsureAttachmentViewPath(profile_dir, thread_id, content_hash, mime, filename, [&]() {
    return LoadAttachmentPlaintext(content, content_hash, dek);
  });
}

Roe<std::string> EnsureAttachmentPosterFromContent(const std::string& profile_dir, const CasStore& content,
                                                   const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                                   const std::string& mime, const std::string& filename,
                                                   const ByteVector& dek, const uint64_t known_byte_length,
                                                   const uint64_t max_receive_bytes) {
  return EnsureAttachmentPoster(
      profile_dir, thread_id, content_hash, mime, filename,
      [&]() { return LoadAttachmentPlaintext(content, content_hash, dek); }, known_byte_length,
      max_receive_bytes);
}

Roe<std::string> EnsureAttachmentImageThumbnailFromContent(const std::string& profile_dir, const CasStore& content,
                                                           const std::string& thread_id,
                                                           const std::vector<uint8_t>& content_hash,
                                                           const std::string& mime, const std::string& filename,
                                                           const ByteVector& dek) {
  // Straight from the store, not through the plaintext memory cache: a large picture is not worth keeping there.
  return EnsureAttachmentImageThumbnail(profile_dir, thread_id, content_hash, mime, filename,
                                        [&]() { return LoadPrivateContent(content, content_hash, dek); });
}

Roe<void> CopyAttachmentPlaintext(const std::string& profile_dir, CasStore& content,
                                  const std::string& thread_id, const ChatAttachmentFields& fields,
                                  const ByteVector& plaintext, const ByteVector& dek,
                                  const uint64_t max_receive_bytes) {
  if (plaintext.empty()) {
    return Error("Sent attachment is empty");
  }
  if (auto saved = SaveAttachmentPlaintext(content, fields.content_hash, fields.mime, plaintext, fields.filename, dek);
      !saved) {
    return saved.error();
  }
  const uint64_t size = fields.byte_length > 0 ? fields.byte_length : plaintext.size();
  // A large file we sent is kept encrypted only; its view is made by an Open, as for a received one.
  if (size <= kMaxDirectAttachmentBytes && AttachmentAllowsInlinePrivateView(fields.mime, size, max_receive_bytes)) {
    (void)StoreAttachmentViewBytes(profile_dir, thread_id, fields.content_hash, fields.mime, fields.filename,
                                   plaintext);
  }
  return {};
}

} // namespace pbr
