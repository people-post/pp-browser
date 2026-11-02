#include "lib/messaging/ChatAttachmentPrepare.h"

#include "lib/delivery/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "lib/messaging/AttachmentCache.h"

#include "common/PbrCompat.h"

namespace pbr {

Roe<PreparedChatAttachment> PrepareChatAttachmentFromBytes(const std::vector<uint8_t>& plaintext,
                                                           const std::string& filename, const std::string& mime,
                                                           const uint64_t max_bytes) {
  if (plaintext.empty()) {
    return Error("Attachment file is empty");
  }
  if (plaintext.size() > max_bytes) {
    return Error("Attachment is over the size limit");
  }

  auto content_key = AttachmentContentCipher::GenerateContentKey();
  if (!content_key) {
    return content_key.error();
  }
  auto encrypted = AttachmentContentCipher::Encrypt(*content_key, plaintext);
  if (!encrypted) {
    return encrypted.error();
  }
  auto content_hash = AttachmentContentHash(plaintext);
  if (!content_hash) {
    return content_hash.error();
  }

  PreparedChatAttachment prepared;
  prepared.fields.filename = filename.empty() ? "attachment" : filename;
  prepared.fields.mime = mime.empty() ? AttachmentMimeFromFilename(prepared.fields.filename) : mime;
  prepared.fields.byte_length = plaintext.size();
  prepared.fields.content_hash = *content_hash;
  prepared.fields.content_key = *content_key;
  prepared.fields.blob_nonce = encrypted->nonce;
  prepared.ciphertext = std::move(encrypted->ciphertext); // no second copy
  return prepared;
}

} // namespace pbr
