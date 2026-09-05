#include "domain/messaging/AttachmentCache.h"

#include "domain/messaging/AttachmentPlaintextMemoryCache.h"
#include "domain/messaging/CasStore.h"

#include "foundation/crypto/CryptoConstants.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/platform/VideoPosterExtractor.h"
#include "foundation/crypto/CryptoUtil.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

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


ByteVector ContentIdFromHash(const std::vector<uint8_t>& content_hash) {
  return ByteVector(content_hash.begin(), content_hash.end());
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

bool AttachmentAllowsInlinePrivateView(const std::string& mime, const uint64_t byte_length) {
  if (!IsAttachmentVideoMime(mime)) {
    return true;
  }
  if (byte_length == 0) {
    return true;
  }
  return byte_length <= kMaxInlinePrivateVideoBytes;
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
  (void)thread_id;
  if (profile_dir.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return false;
  }
  const ByteVector content_id = ContentIdFromHash(content_hash);
  CasStore cas(profile_dir, {});
  return cas.Exists(CasRealm::Private, content_id);
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
  return {};
}

Roe<std::string> SaveAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                         const std::vector<uint8_t>& content_hash, const std::string& mime,
                                         const ByteVector& plaintext, const std::string& filename,
                                         const ByteVector& dek, std::string_view profile_id) {
  (void)thread_id;
  if (profile_dir.empty()) {
    return Error("Attachment cache profile directory is required");
  }
  if (profile_id.empty()) {
    return Error("Attachment CAS save requires profile_id");
  }
  if (content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash");
  }
  if (plaintext.empty()) {
    return Error("Attachment plaintext is empty");
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size for attachment CAS save");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  if (hash.value() != content_hash) {
    return Error("Attachment plaintext hash mismatch");
  }

  const ByteVector content_id = ContentIdFromHash(content_hash);
  CasStore cas(profile_dir, std::string(profile_id));
  if (auto put = cas.PutPrivate(content_id, plaintext, dek, mime, filename); !put) {
    return put.error();
  }
  AttachmentPlaintextMemoryCache::Instance().Put(AttachmentHashHex(content_hash), plaintext);
  return cas.BlockPath(CasRealm::Private, content_id);
}

Roe<ByteVector> LoadAttachmentPlaintext(const std::string& profile_dir, const std::string& thread_id,
                                        const std::vector<uint8_t>& content_hash, const std::string& mime,
                                        const std::string& filename, const ByteVector& dek,
                                        std::string_view profile_id) {
  (void)thread_id;
  (void)mime;
  (void)filename;
  if (profile_dir.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment load lookup");
  }
  if (profile_id.empty()) {
    return Error("Attachment CAS load requires profile_id");
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size for attachment CAS load");
  }
  const std::string hash_hex = AttachmentHashHex(content_hash);
  ByteVector cached;
  if (AttachmentPlaintextMemoryCache::Instance().TryGet(hash_hex, cached)) {
    return cached;
  }
  const ByteVector content_id = ContentIdFromHash(content_hash);
  CasStore cas(profile_dir, std::string(profile_id));
  if (!cas.Exists(CasRealm::Private, content_id)) {
    return Error("Attachment not cached in private CAS");
  }
  auto plain = cas.GetPrivate(content_id, dek);
  if (!plain) {
    return plain.error();
  }
  AttachmentPlaintextMemoryCache::Instance().Put(hash_hex, *plain);
  return plain;
}

Roe<std::string> EnsureAttachmentViewPath(const std::string& profile_dir, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash, const std::string& mime,
                                          const std::string& filename, const ByteVector& dek,
                                          std::string_view profile_id) {
  if (profile_dir.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment view lookup");
  }

  const auto view_root = std::filesystem::path(AttachmentViewRoot(profile_dir, thread_id));
  if (const auto existing_view = FindBlobFile(view_root, content_hash, mime, filename); !existing_view.empty()) {
    return existing_view.string();
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
                                        const std::string& filename, const ByteVector& dek,
                                        std::string_view profile_id, const uint64_t known_byte_length) {
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

  Roe<std::vector<uint8_t>> jpeg = [&]() -> Roe<std::vector<uint8_t>> {
    if (!AttachmentAllowsInlinePrivateView(mime, known_byte_length)) {
      return SoftVideoPosterJpeg();
    }
    auto view = EnsureAttachmentViewPath(profile_dir, thread_id, content_hash, mime, filename, dek, profile_id);
    if (!view) {
      return view.error();
    }
    return ExtractVideoPosterJpeg(*view);
  }();
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
  AttachmentPlaintextMemoryCache::Instance().Put(AttachmentHashHex(content_hash) + ".poster.jpg", bytes);
  return poster_path;
}

Roe<void> CopyAttachmentPlaintextFile(const std::string& profile_dir, const std::string& thread_id,
                                      const ChatAttachmentFields& fields, const std::string& source_path,
                                      const ByteVector& dek, std::string_view profile_id) {
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
  if (AttachmentAllowsInlinePrivateView(fields.mime, fields.byte_length > 0 ? fields.byte_length
                                                                            : plaintext.size())) {
    (void)EnsureAttachmentViewPath(profile_dir, thread_id, fields.content_hash, fields.mime, fields.filename, dek,
                                   profile_id);
  }
  return {};
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
  AttachmentPlaintextMemoryCache::Instance().Clear();
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
  uint64_t total = DirectoryTreeByteSize(std::filesystem::path(profile_dir) / "cas" / "private");
  const auto threads_root = std::filesystem::path(ThreadsRoot(profile_dir));
  std::error_code ec;
  if (!std::filesystem::exists(threads_root, ec) || ec) {
    return total;
  }

  for (const auto& entry : std::filesystem::directory_iterator(threads_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    total += DirectoryTreeByteSize(entry.path() / "blobs_view");
    total += DirectoryTreeByteSize(entry.path() / "blob_cipher");
  }
  return total;
}

Roe<void> WipeAllAttachmentCaches(const std::string& profile_dir) {
  AttachmentPlaintextMemoryCache::Instance().Clear();
  if (profile_dir.empty()) {
    return Error("Attachment cache profile directory is required");
  }
  const auto threads_root = std::filesystem::path(ThreadsRoot(profile_dir));
  std::error_code ec;
  if (std::filesystem::exists(threads_root, ec)) {
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
  }
  const auto private_cas = std::filesystem::path(profile_dir) / "cas" / "private";
  if (std::filesystem::exists(private_cas, ec)) {
    std::filesystem::remove_all(private_cas, ec);
    if (ec) {
      return Error("Failed to wipe private CAS blocks");
    }
  }
  // P2: only private CAS objects exist from attachments; drop the index file.
  for (const char* name : {"object_index.db", "object_index.db-wal", "object_index.db-shm"}) {
    std::filesystem::remove(std::filesystem::path(profile_dir) / name, ec);
  }
  return {};
}


} // namespace pbr
