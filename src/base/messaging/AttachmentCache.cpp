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

uint64_t DirectoryTreeByteSize(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return 0;
  }
  uint64_t total = 0;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, options, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const uint64_t size = entry.file_size(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    total += size;
  }
  return total;
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

Roe<void> WipeThreadAttachmentBlobs(const std::string& profile_dir, const std::string& thread_id) {
  if (profile_dir.empty() || thread_id.empty()) {
    return Error("Attachment cache profile directory and thread_id are required");
  }
  const auto root = std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id));
  std::error_code ec;
  if (std::filesystem::exists(root, ec)) {
    std::filesystem::remove_all(root, ec);
    if (ec) {
      return Error("Failed to wipe thread attachment blobs");
    }
  }
  const auto pending = std::filesystem::path(AttachmentPendingCiphertextRoot(profile_dir, thread_id));
  if (std::filesystem::exists(pending, ec)) {
    std::filesystem::remove_all(pending, ec);
    if (ec) {
      return Error("Failed to wipe pending attachment ciphertext");
    }
  }
  return Roe<void>{};
}

std::string AttachmentPendingCiphertextRoot(const std::string& profile_dir, const std::string& thread_id) {
  return (std::filesystem::path(ThreadsRoot(profile_dir)) / thread_id / "blob_cipher").string();
}

Roe<void> SavePendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash,
                                          const std::vector<uint8_t>& ciphertext) {
  if (profile_dir.empty() || thread_id.empty()) {
    return Error("Attachment cache profile directory and thread_id are required");
  }
  if (content_hash.size() != kAttachmentContentHashSize || ciphertext.empty()) {
    return Error("Invalid pending attachment ciphertext");
  }
  const auto root = std::filesystem::path(AttachmentPendingCiphertextRoot(profile_dir, thread_id));
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Error("Failed to create pending attachment directory");
  }
  const auto path = root / AttachmentHashHex(content_hash);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return Error("Failed to write pending attachment ciphertext");
  }
  output.write(reinterpret_cast<const char*>(ciphertext.data()), static_cast<std::streamsize>(ciphertext.size()));
  if (!output) {
    return Error("Failed to write pending attachment ciphertext");
  }
  return Roe<void>{};
}


bool AttachmentPendingCiphertextExists(const std::string& profile_dir, const std::string& thread_id,
                                       const std::vector<uint8_t>& content_hash) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return false;
  }
  const auto path = std::filesystem::path(AttachmentPendingCiphertextRoot(profile_dir, thread_id)) /
                    AttachmentHashHex(content_hash);
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

Roe<ByteVector> LoadPendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                                const std::vector<uint8_t>& content_hash) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid pending attachment lookup");
  }
  const auto path = std::filesystem::path(AttachmentPendingCiphertextRoot(profile_dir, thread_id)) /
                    AttachmentHashHex(content_hash);
  if (!std::filesystem::exists(path)) {
    return Error("Pending attachment ciphertext not found");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Failed to read pending attachment ciphertext");
  }
  return ByteVector((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void RemovePendingAttachmentCiphertext(const std::string& profile_dir, const std::string& thread_id,
                                       const std::vector<uint8_t>& content_hash) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return;
  }
  const auto path = std::filesystem::path(AttachmentPendingCiphertextRoot(profile_dir, thread_id)) /
                    AttachmentHashHex(content_hash);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

uint64_t AttachmentCacheByteSize(const std::string& profile_dir) {
  if (profile_dir.empty()) {
    return 0;
  }
  const auto threads_root = std::filesystem::path(ThreadsRoot(profile_dir));
  std::error_code ec;
  if (!std::filesystem::exists(threads_root, ec) || ec) {
    return 0;
  }

  uint64_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(threads_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    total += DirectoryTreeByteSize(entry.path() / "blobs");
    total += DirectoryTreeByteSize(entry.path() / "blob_cipher");
  }
  return total;
}

Roe<void> WipeAllAttachmentCaches(const std::string& profile_dir) {
  if (profile_dir.empty()) {
    return Error("Attachment cache profile directory is required");
  }
  const auto threads_root = std::filesystem::path(ThreadsRoot(profile_dir));
  std::error_code ec;
  if (!std::filesystem::exists(threads_root, ec)) {
    return Roe<void>{};
  }
  for (const auto& entry : std::filesystem::directory_iterator(threads_root, ec)) {
    if (ec) {
      return Error("Failed to enumerate thread attachment caches");
    }
    if (!entry.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    if (auto wiped = WipeThreadAttachmentBlobs(profile_dir, entry.path().filename().string()); !wiped) {
      return wiped.error();
    }
  }
  return Roe<void>{};
}

} // namespace pbr
