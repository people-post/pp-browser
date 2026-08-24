#include "base/messaging/AttachmentCache.h"

#include "base/crypto/AttachmentContentHash.h"
#include "base/crypto/CryptoUtil.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace pbr {

namespace {

std::string ThreadsRoot(const std::string& profile_dir) {
  return (std::filesystem::path(profile_dir) / "threads").string();
}

std::string AttachmentFilename(const std::vector<uint8_t>& content_hash, const std::string& mime,
                               const std::string& filename) {
  const std::string ext = AttachmentExtensionFromMime(mime, filename);
  if (ext.empty()) {
    return AttachmentHashHex(content_hash);
  }
  return AttachmentHashHex(content_hash) + "." + ext;
}

} // namespace

std::string AttachmentBlobRoot(const std::string& profile_dir, const std::string& thread_id) {
  return (std::filesystem::path(ThreadsRoot(profile_dir)) / thread_id / "blobs").string();
}

std::string AttachmentHashHex(const std::vector<uint8_t>& content_hash) {
  return BytesToHex(ByteVector(content_hash.begin(), content_hash.end()));
}

std::string AttachmentExtensionFromMime(const std::string& mime, const std::string& filename) {
  if (!filename.empty()) {
    const std::string ext = std::filesystem::path(filename).extension().string();
    if (!ext.empty() && ext.size() <= 8) {
      return ext.size() > 1 && ext[0] == '.' ? ext.substr(1) : ext;
    }
  }
  if (mime == "image/png") {
    return "png";
  }
  if (mime == "image/jpeg" || mime == "image/jpg") {
    return "jpg";
  }
  if (mime == "image/webp") {
    return "webp";
  }
  if (mime == "image/gif") {
    return "gif";
  }
  if (mime == "video/mp4") {
    return "mp4";
  }
  if (mime == "video/webm") {
    return "webm";
  }
  if (mime == "application/pdf") {
    return "pdf";
  }
  if (mime == "text/plain") {
    return "txt";
  }
  return {};
}

bool IsAttachmentImageMime(const std::string& mime) {
  return mime.rfind("image/", 0) == 0;
}

bool IsAttachmentVideoMime(const std::string& mime) {
  return mime.rfind("video/", 0) == 0;
}

bool AttachmentOpenNeedsConfirm(const std::string& mime) {
  return !IsAttachmentImageMime(mime) && !IsAttachmentVideoMime(mime);
}

std::string FormatAttachmentByteSize(const uint64_t byte_length) {
  if (byte_length < 1024) {
    return std::to_string(byte_length) + " B";
  }
  if (byte_length < 1024 * 1024) {
    return std::to_string(byte_length / 1024) + " KiB";
  }
  return std::to_string(byte_length / (1024 * 1024)) + " MiB";
}

std::string AttachmentLocalPath(const std::string& profile_dir, const std::string& thread_id,
                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                const std::string& filename) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return {};
  }
  const auto root = std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id));
  const std::string hash_name = AttachmentHashHex(content_hash);
  const std::string ext = AttachmentExtensionFromMime(mime, filename);

  const auto with_ext = root / AttachmentFilename(content_hash, mime, filename);
  if (std::filesystem::exists(with_ext)) {
    return with_ext.string();
  }
  const auto bare = root / hash_name;
  if (std::filesystem::exists(bare)) {
    return bare.string();
  }
  if (!ext.empty()) {
    const auto alt = root / (hash_name + "." + ext);
    if (std::filesystem::exists(alt)) {
      return alt.string();
    }
  }
  return {};
}

Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename) {
  if (profile_dir.empty() || thread_id.empty()) {
    return Error("Attachment cache profile directory is required");
  }
  if (content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash");
  }
  if (plaintext.empty()) {
    return Error("Attachment plaintext is empty");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  if (hash.value() != content_hash) {
    return Error("Attachment plaintext hash mismatch");
  }

  const auto root = std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id));
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Error("Failed to create attachment cache directory");
  }

  const auto path = root / AttachmentFilename(content_hash, mime, filename);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Error("Failed to write attachment cache file");
  }
  out.write(reinterpret_cast<const char*>(plaintext.data()), static_cast<std::streamsize>(plaintext.size()));
  if (!out) {
    return Error("Failed to write attachment cache file");
  }
  return path.string();
}

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, const std::string& thread_id,
                                      const ChatAttachmentFields& fields, const std::string& source_path) {
  std::ifstream input(source_path, std::ios::binary);
  if (!input) {
    return Error("Could not read sent attachment file");
  }
  const ByteVector plaintext((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (plaintext.empty()) {
    return Error("Sent attachment file is empty");
  }
  if (auto saved = SaveAttachmentPlaintext(profile_dir, thread_id, fields.content_hash, fields.mime, plaintext,
                                           fields.filename);
      !saved) {
    return saved.error();
  }
  return Roe<void>{};
}

} // namespace pbr
