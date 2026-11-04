#include "lib/content/PrivateContent.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"

#include "common/PbrCompat.h"

namespace pbr {

namespace {

ByteVector ContentIdFromHash(const std::vector<uint8_t>& content_hash) {
  return ByteVector(content_hash.begin(), content_hash.end());
}


} // namespace

bool PrivateContentExists(const CasStore& content, const std::vector<uint8_t>& content_hash) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return false;
  }
  return content.Exists(CasRealm::Private, ContentIdFromHash(content_hash));
}

Roe<std::string> SavePrivateContent(CasStore& content, const std::vector<uint8_t>& content_hash, const std::string& mime,
                                    const ByteVector& plaintext, const std::string& filename, const ByteVector& dek) {
  if (content.ProfileId().empty()) {
    return Error("Private content save requires profile_id");
  }
  if (content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid content hash");
  }
  if (plaintext.empty()) {
    return Error("Content plaintext is empty");
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size for private content save");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  if (hash.value() != content_hash) {
    return Error("Content plaintext hash mismatch");
  }
  const ByteVector content_id = ContentIdFromHash(content_hash);
  if (auto put = content.PutPrivate(content_id, plaintext, dek, mime, filename); !put) {
    return put.error();
  }
  return content.BlockPath(CasRealm::Private, content_id);
}

Roe<ByteVector> LoadPrivateContent(const CasStore& content, const std::vector<uint8_t>& content_hash,
                                   const ByteVector& dek) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid private content lookup");
  }
  if (content.ProfileId().empty()) {
    return Error("Private content load requires profile_id");
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size for private content load");
  }
  const ByteVector content_id = ContentIdFromHash(content_hash);
  if (!content.Exists(CasRealm::Private, content_id)) {
    return Error("Content not in private CAS");
  }
  return content.GetPrivate(content_id, dek);
}

} // namespace pbr
