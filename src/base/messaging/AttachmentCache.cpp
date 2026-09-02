#include "base/messaging/AttachmentCache.h"

#include "base/crypto/AttachmentContentHash.h"
#include "foundation/platform/VideoPosterExtractor.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/FileCipher.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr char kAttachmentBlobMagic[4] = {'P', 'P', 'B', 'A'};

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

Roe<ByteVector> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Failed to read attachment cache file");
  }
  return ByteVector((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool FileStartsWithMagic(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  char magic[4] = {};
  input.read(magic, 4);
  if (input.gcount() != 4) {
    return false;
  }
  return std::memcmp(magic, kAttachmentBlobMagic, 4) == 0;
}

std::filesystem::path FindBlobFile(const std::filesystem::path& root, const std::vector<uint8_t>& content_hash,
                                   const std::string& mime, const std::string& filename) {
  if (!std::filesystem::exists(root)) {
    return {};
  }
  const std::string hash_name = AttachmentHashHex(content_hash);
  const std::string ext = AttachmentExtensionFromMime(mime, filename);

  const auto with_ext = root / AttachmentFilename(content_hash, mime, filename);
  if (std::filesystem::exists(with_ext) && std::filesystem::is_regular_file(with_ext)) {
    return with_ext;
  }
  const auto bare = root / hash_name;
  if (std::filesystem::exists(bare) && std::filesystem::is_regular_file(bare)) {
    return bare;
  }
  if (!ext.empty()) {
    const auto alt = root / (hash_name + "." + ext);
    if (std::filesystem::exists(alt) && std::filesystem::is_regular_file(alt)) {
      return alt;
    }
  }

  // Any file whose stem starts with hash (extension may differ from mime).
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec || !entry.is_regular_file(ec)) {
      ec.clear();
      continue;
    }
    const std::string name = entry.path().filename().string();
    // Skip session poster sidecars (`{hash}.poster.jpg`) — not attachment payloads.
    if (name.size() > 11 && name.compare(name.size() - 11, 11, ".poster.jpg") == 0) {
      continue;
    }
    if (name == hash_name || name.rfind(hash_name + ".", 0) == 0) {
      return entry.path();
    }
  }
  return {};
}

Roe<void> WriteBytesAtomic(const std::filesystem::path& path, const ByteVector& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return Error("Failed to create attachment cache directory");
  }
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Error("Failed to write attachment cache file");
    }
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
      return Error("Failed to write attachment cache file");
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp);
    return Error("Failed to finalize attachment cache file");
  }
  return {};
}

} // namespace

std::string AttachmentBlobRoot(const std::string& profile_dir, const std::string& thread_id) {
  return (std::filesystem::path(ThreadsRoot(profile_dir)) / thread_id / "blobs").string();
}

std::string AttachmentViewRoot(const std::string& profile_dir, const std::string& thread_id) {
  return (std::filesystem::path(ThreadsRoot(profile_dir)) / thread_id / "blobs_view").string();
}

std::string AttachmentHashHex(const std::vector<uint8_t>& content_hash) {
  return BytesToHex(ByteVector(content_hash.begin(), content_hash.end()));
}

std::string BuildAttachmentBlobAad(std::string_view profile_id, std::string_view thread_id,
                                   std::string_view hash_hex) {
  return std::string("attachment-blob|") + std::string(profile_id) + "|" + std::string(thread_id) + "|" +
         std::string(hash_hex) + "|1";
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

bool AttachmentBlobExists(const std::string& profile_dir, const std::string& thread_id,
                          const std::vector<uint8_t>& content_hash) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return false;
  }
  const auto path = FindBlobFile(std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id)), content_hash, "",
                                 {});
  return !path.empty();
}

std::string AttachmentLocalPath(const std::string& profile_dir, const std::string& thread_id,
                                const std::vector<uint8_t>& content_hash, const std::string& mime,
                                const std::string& filename) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return {};
  }

  const auto view_root = std::filesystem::path(AttachmentViewRoot(profile_dir, thread_id));
  if (const auto view = FindBlobFile(view_root, content_hash, mime, filename); !view.empty()) {
    return view.string();
  }

  const auto blob_root = std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id));
  const auto blob = FindBlobFile(blob_root, content_hash, mime, filename);
  if (blob.empty()) {
    return {};
  }
  // Never hand ciphertext / PPBA-wrapped files to UI or OS open.
  if (FileStartsWithMagic(blob)) {
    return {};
  }
  return blob.string();
}

Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename,
                                         const ByteVector* dek, std::string_view profile_id) {
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
  if (dek != nullptr) {
    if (profile_id.empty()) {
      return Error("Attachment DEK-wrap requires profile_id");
    }
    const std::string aad = BuildAttachmentBlobAad(profile_id, thread_id, AttachmentHashHex(content_hash));
    auto cipher = FileCipher::Encrypt(*dek, plaintext, aad);
    if (!cipher) {
      return cipher.error();
    }
    ByteVector on_disk;
    on_disk.reserve(4 + cipher->size());
    on_disk.insert(on_disk.end(), kAttachmentBlobMagic, kAttachmentBlobMagic + 4);
    on_disk.insert(on_disk.end(), cipher->begin(), cipher->end());
    if (auto written = WriteBytesAtomic(path, on_disk); !written) {
      return written.error();
    }
  } else {
    if (auto written = WriteBytesAtomic(path, plaintext); !written) {
      return written.error();
    }
  }
  return path.string();
}

Roe<ByteVector> LoadAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename, const ByteVector* dek,
                                        std::string_view profile_id) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment load lookup");
  }
  const auto path =
      FindBlobFile(std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id)), content_hash, mime, filename);
  if (path.empty()) {
    return Error("Attachment blob not cached locally");
  }
  auto raw = ReadFileBytes(path);
  if (!raw) {
    return raw.error();
  }
  if (raw->size() >= 4 && std::memcmp(raw->data(), kAttachmentBlobMagic, 4) == 0) {
    if (dek == nullptr) {
      return Error("Attachment blob is DEK-wrapped; unlock required");
    }
    if (profile_id.empty()) {
      return Error("Attachment DEK unwrap requires profile_id");
    }
    const ByteVector blob(raw->begin() + 4, raw->end());
    const std::string aad = BuildAttachmentBlobAad(profile_id, thread_id, AttachmentHashHex(content_hash));
    auto plain = FileCipher::Decrypt(*dek, blob, aad);
    if (!plain) {
      return plain.error();
    }
    auto hash = AttachmentContentHash(*plain);
    if (!hash) {
      return hash.error();
    }
    if (*hash != content_hash) {
      return Error("Attachment plaintext hash mismatch after decrypt");
    }
    return plain;
  }
  return raw;
}

Roe<std::string> EnsureAttachmentViewPath(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash, const std::string& mime,
                                          const std::string& filename, const ByteVector* dek,
                                          std::string_view profile_id) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment view lookup");
  }

  const auto view_root = std::filesystem::path(AttachmentViewRoot(profile_dir, thread_id));
  if (const auto existing_view = FindBlobFile(view_root, content_hash, mime, filename); !existing_view.empty()) {
    return existing_view.string();
  }

  const auto blob_root = std::filesystem::path(AttachmentBlobRoot(profile_dir, thread_id));
  const auto blob_path = FindBlobFile(blob_root, content_hash, mime, filename);
  if (blob_path.empty()) {
    return Error("Attachment blob not cached locally");
  }

  if (!FileStartsWithMagic(blob_path)) {
    // Legacy plaintext under blobs/ is already usable.
    return blob_path.string();
  }

  auto plain = LoadAttachmentPlaintext(profile_dir, thread_id, content_hash, mime, filename, dek, profile_id);
  if (!plain) {
    return plain.error();
  }

  const auto view_path = view_root / AttachmentFilename(content_hash, mime, filename);
  if (auto written = WriteBytesAtomic(view_path, *plain); !written) {
    return written.error();
  }
  return view_path.string();
}

std::string AttachmentPosterPath(const std::string& profile_dir, const std::string& thread_id,
                                 const std::vector<uint8_t>& content_hash) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return {};
  }
  return (std::filesystem::path(AttachmentViewRoot(profile_dir, thread_id)) /
          (AttachmentHashHex(content_hash) + ".poster.jpg"))
      .string();
}

bool AttachmentPosterExists(const std::string& profile_dir, const std::string& thread_id,
                            const std::vector<uint8_t>& content_hash) {
  const std::string path = AttachmentPosterPath(profile_dir, thread_id, content_hash);
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

Roe<std::string> EnsureAttachmentPoster(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename, const ByteVector* dek,
                                        std::string_view profile_id) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment poster lookup");
  }
  const std::string poster_path = AttachmentPosterPath(profile_dir, thread_id, content_hash);
  if (poster_path.empty()) {
    return Error("Invalid attachment poster path");
  }
  std::error_code ec;
  if (std::filesystem::is_regular_file(poster_path, ec) && !ec) {
    return poster_path;
  }
  if (!IsAttachmentVideoMime(mime)) {
    return Error("Attachment is not video; no poster");
  }

  auto view = EnsureAttachmentViewPath(profile_dir, thread_id, content_hash, mime, filename, dek, profile_id);
  if (!view) {
    return view.error();
  }

  auto jpeg = ExtractVideoPosterJpeg(*view);
  if (!jpeg) {
    return jpeg.error();
  }
  if (jpeg->empty()) {
    return Error("Video poster JPEG empty");
  }

  const auto view_root = std::filesystem::path(AttachmentViewRoot(profile_dir, thread_id));
  std::filesystem::create_directories(view_root, ec);
  if (ec) {
    return Error("Failed to create attachment view directory for poster");
  }

  const ByteVector bytes(jpeg->begin(), jpeg->end());
  if (auto written = WriteBytesAtomic(std::filesystem::path(poster_path), bytes); !written) {
    return written.error();
  }
  return poster_path;
}

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, const std::string& thread_id,
                                      const ChatAttachmentFields& fields, const std::string& source_path,
                                      const ByteVector* dek, std::string_view profile_id) {
  std::ifstream input(source_path, std::ios::binary);
  if (!input) {
    return Error("Could not read sent attachment file");
  }
  const ByteVector plaintext((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (plaintext.empty()) {
    return Error("Sent attachment file is empty");
  }
  if (auto saved = SaveAttachmentPlaintext(profile_dir, thread_id, fields.content_hash, fields.mime, plaintext,
                                           fields.filename, dek, profile_id);
      !saved) {
    return saved.error();
  }
  // Materialize view for immediate display when wrapped.
  if (dek != nullptr) {
    (void)EnsureAttachmentViewPath(profile_dir, thread_id, fields.content_hash, fields.mime, fields.filename, dek,
                                   profile_id);
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
  const auto view = std::filesystem::path(AttachmentViewRoot(profile_dir, thread_id));
  if (std::filesystem::exists(view, ec)) {
    std::filesystem::remove_all(view, ec);
    if (ec) {
      return Error("Failed to wipe thread attachment view cache");
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

Roe<void> WipeAllAttachmentViewCaches(const std::string& profile_dir) {
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
      return Error("Failed to enumerate thread attachment view caches");
    }
    if (!entry.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    const auto view = entry.path() / "blobs_view";
    if (std::filesystem::exists(view, ec)) {
      std::filesystem::remove_all(view, ec);
      if (ec) {
        return Error("Failed to wipe attachment view cache");
      }
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
    total += DirectoryTreeByteSize(entry.path() / "blobs_view");
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
