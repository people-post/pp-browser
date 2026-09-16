#include "domain/messaging/ChatAttachmentPrepare.h"

#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<ByteVector> ReadFileBytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Could not read attachment file");
  }
  const ByteVector bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    return Error("Attachment file is empty");
  }
  if (bytes.size() > kMaxChatAttachmentPlaintextBytes) {
    return Error("Attachment exceeds 4 MiB limit");
  }
  return bytes;
}

std::string FilenameFromPath(const std::string& path) {
  const std::filesystem::path file_path(path);
  if (file_path.has_filename()) {
    return file_path.filename().string();
  }
  return "attachment";
}

std::string MimeFromFilename(const std::string& filename) {
  std::string ext = std::filesystem::path(filename).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (ext == ".png") {
    return "image/png";
  }
  if (ext == ".jpg" || ext == ".jpeg") {
    return "image/jpeg";
  }
  if (ext == ".webp") {
    return "image/webp";
  }
  if (ext == ".gif") {
    return "image/gif";
  }
  if (ext == ".pdf") {
    return "application/pdf";
  }
  if (ext == ".txt") {
    return "text/plain";
  }
  if (ext == ".mp4") {
    return "video/mp4";
  }
  if (ext == ".webm") {
    return "video/webm";
  }
  return "application/octet-stream";
}

} // namespace

Roe<PreparedChatAttachment> PrepareChatAttachmentFromFile(const std::string& path) {
  auto plaintext = ReadFileBytes(path);
  if (!plaintext) {
    return plaintext.error();
  }

  auto content_key = AttachmentContentCipher::GenerateContentKey();
  if (!content_key) {
    return content_key.error();
  }
  auto encrypted = AttachmentContentCipher::Encrypt(*content_key, *plaintext);
  if (!encrypted) {
    return encrypted.error();
  }
  auto content_hash = AttachmentContentHash(*plaintext);
  if (!content_hash) {
    return content_hash.error();
  }

  PreparedChatAttachment prepared;
  prepared.fields.mime = MimeFromFilename(FilenameFromPath(path));
  prepared.fields.filename = FilenameFromPath(path);
  prepared.fields.byte_length = plaintext->size();
  prepared.fields.content_hash = *content_hash;
  prepared.fields.content_key = *content_key;
  prepared.fields.blob_nonce = encrypted->nonce;
  prepared.ciphertext.assign(encrypted->ciphertext.begin(), encrypted->ciphertext.end());
  return prepared;
}

} // namespace pbr
