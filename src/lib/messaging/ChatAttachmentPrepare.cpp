#include "lib/messaging/ChatAttachmentPrepare.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "lib/messaging/AttachmentCache.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<ByteVector> ReadFileBytes(const std::string& path, const uint64_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec && size > max_bytes) {
    return Error("Attachment is over the size limit"); // before the file is read
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Could not read attachment file");
  }
  const ByteVector bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return bytes;
}

std::string FilenameFromPath(const std::string& path) {
  const std::filesystem::path file_path(path);
  if (file_path.has_filename()) {
    return file_path.filename().string();
  }
  return "attachment";
}

} // namespace

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
  prepared.ciphertext.assign(encrypted->ciphertext.begin(), encrypted->ciphertext.end());
  return prepared;
}

Roe<PreparedChatAttachment> PrepareChatAttachmentFromFile(const std::string& path, const uint64_t max_bytes) {
  auto plaintext = ReadFileBytes(path, max_bytes);
  if (!plaintext) {
    return plaintext.error();
  }
  return PrepareChatAttachmentFromBytes(*plaintext, FilenameFromPath(path), std::string(), max_bytes);
}

} // namespace pbr
