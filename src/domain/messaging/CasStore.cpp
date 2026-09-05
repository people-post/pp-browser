#include "domain/messaging/CasStore.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "common/PbrCompat.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace pbr {
namespace {

constexpr char kPpbaMagic[4] = {'P', 'P', 'B', 'A'};

Roe<void> ValidateContentId(const ByteVector& content_id) {
  if (content_id.size() != kCasContentIdSize) {
    return Error("Invalid CAS content id");
  }
  return {};
}

Roe<void> VerifyContentIdMatchesPlaintext(const ByteVector& content_id, const ByteVector& plaintext) {
  auto hashed = AttachmentContentHash(plaintext);
  if (!hashed) {
    return hashed.error();
  }
  if (*hashed != content_id) {
    return Error("CAS content id does not match plaintext hash");
  }
  return {};
}

} // namespace

std::string BuildCasPrivateAad(const std::string_view profile_id, const std::string_view content_id_hex) {
  return std::string("cas-private|") + std::string(profile_id) + "|" + std::string(content_id_hex) + "|1";
}

CasStore::CasStore(std::string profile_dir, std::string profile_id)
    : profile_dir_(std::move(profile_dir)), profile_id_(std::move(profile_id)), index_(profile_dir_) {}

std::string CasStore::BlocksRoot(const CasRealm realm) const {
  return (std::filesystem::path(profile_dir_) / "cas" / CasRealmToString(realm) / "blocks").string();
}

std::string CasStore::BlockPath(const CasRealm realm, const ByteVector& content_id) const {
  return (std::filesystem::path(BlocksRoot(realm)) / BytesToHex(content_id)).string();
}

bool CasStore::Exists(const CasRealm realm, const ByteVector& content_id) const {
  if (content_id.size() != kCasContentIdSize || profile_dir_.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(BlockPath(realm, content_id), ec) && !ec;
}

Roe<void> CasStore::WriteBlockFile(const std::string& path, const ByteVector& bytes) const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  if (ec) {
    return Error("Failed to create CAS block directory");
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Error("Failed to write CAS block");
    }
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
      return Error("Failed to write CAS block");
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp);
    return Error("Failed to finalize CAS block");
  }
  return {};
}

Roe<ByteVector> CasStore::ReadBlockFile(const std::string& path) const {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error("CAS block not found");
  }
  return ByteVector((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Roe<void> CasStore::PutPrivate(const ByteVector& content_id, const ByteVector& plaintext, const ByteVector& dek,
                               const std::string_view mime, const std::string_view filename, const bool pinned) {
  if (profile_dir_.empty() || profile_id_.empty()) {
    return Error("CAS private put requires profile_dir and profile_id");
  }
  if (auto valid = ValidateContentId(content_id); !valid) {
    return valid.error();
  }
  if (plaintext.empty()) {
    return Error("CAS private plaintext is empty");
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size for CAS private put");
  }
  if (auto verified = VerifyContentIdMatchesPlaintext(content_id, plaintext); !verified) {
    return verified.error();
  }

  const std::string id_hex = BytesToHex(content_id);
  const std::string aad = BuildCasPrivateAad(profile_id_, id_hex);
  auto cipher = FileCipher::Encrypt(dek, plaintext, aad);
  if (!cipher) {
    return cipher.error();
  }
  ByteVector on_disk;
  on_disk.reserve(4 + cipher->size());
  on_disk.insert(on_disk.end(), kPpbaMagic, kPpbaMagic + 4);
  on_disk.insert(on_disk.end(), cipher->begin(), cipher->end());

  if (auto written = WriteBlockFile(BlockPath(CasRealm::Private, content_id), on_disk); !written) {
    return written.error();
  }

  CasObjectMeta meta;
  meta.realm = CasRealm::Private;
  meta.content_id = content_id;
  meta.mime = std::string(mime);
  meta.filename = std::string(filename);
  meta.byte_length = plaintext.size();
  meta.pinned = pinned;
  return index_.Upsert(meta);
}

Roe<ByteVector> CasStore::GetPrivate(const ByteVector& content_id, const ByteVector& dek) const {
  if (auto valid = ValidateContentId(content_id); !valid) {
    return valid.error();
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size for CAS private get");
  }
  if (profile_id_.empty()) {
    return Error("CAS private get requires profile_id");
  }
  auto raw = ReadBlockFile(BlockPath(CasRealm::Private, content_id));
  if (!raw) {
    return raw.error();
  }
  if (raw->size() < 4 || std::memcmp(raw->data(), kPpbaMagic, 4) != 0) {
    return Error("CAS private block is not PPBA-wrapped");
  }
  const ByteVector blob(raw->begin() + 4, raw->end());
  const std::string aad = BuildCasPrivateAad(profile_id_, BytesToHex(content_id));
  return FileCipher::Decrypt(dek, blob, aad);
}

Roe<void> CasStore::PutPublic(const ByteVector& content_id, const ByteVector& published_bytes,
                              const std::string_view mime, const std::string_view filename,
                              const std::string_view published_from_hex, const bool pinned) {
  if (profile_dir_.empty()) {
    return Error("CAS public put requires profile_dir");
  }
  if (auto valid = ValidateContentId(content_id); !valid) {
    return valid.error();
  }
  if (published_bytes.empty()) {
    return Error("CAS public payload is empty");
  }
  if (auto verified = VerifyContentIdMatchesPlaintext(content_id, published_bytes); !verified) {
    return verified.error();
  }
  if (auto written = WriteBlockFile(BlockPath(CasRealm::Public, content_id), published_bytes); !written) {
    return written.error();
  }

  CasObjectMeta meta;
  meta.realm = CasRealm::Public;
  meta.content_id = content_id;
  meta.mime = std::string(mime);
  meta.filename = std::string(filename);
  meta.byte_length = published_bytes.size();
  meta.published_from_hex = std::string(published_from_hex);
  meta.pinned = pinned;
  return index_.Upsert(meta);
}

Roe<ByteVector> CasStore::GetPublic(const ByteVector& content_id) const {
  if (auto valid = ValidateContentId(content_id); !valid) {
    return valid.error();
  }
  auto raw = ReadBlockFile(BlockPath(CasRealm::Public, content_id));
  if (!raw) {
    return raw.error();
  }
  if (raw->size() >= 4 && std::memcmp(raw->data(), kPpbaMagic, 4) == 0) {
    return Error("CAS public block must not be PPBA-wrapped");
  }
  return raw;
}

Roe<void> CasStore::Delete(const CasRealm realm, const ByteVector& content_id) {
  if (auto valid = ValidateContentId(content_id); !valid) {
    return valid.error();
  }
  std::error_code ec;
  std::filesystem::remove(BlockPath(realm, content_id), ec);
  if (auto removed = index_.Remove(realm, content_id); !removed) {
    return removed.error();
  }
  return {};
}

} // namespace pbr
