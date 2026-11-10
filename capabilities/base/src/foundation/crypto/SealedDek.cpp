#include "foundation/crypto/SealedDek.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/crypto/MessageCipher.h"

#include <algorithm>
#include <fstream>
#include <sodium.h>
#include <system_error>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr uint8_t kEntryVersion = 1;
constexpr size_t kEntrySize = 1 + SealedDek::kKeySize;
constexpr size_t kMaxFileBytes = 4096; // version + nonce + 32-byte DEK + tag: well under this

void Wipe(ByteVector& bytes) {
  if (!bytes.empty()) {
    sodium_memzero(bytes.data(), bytes.size());
  }
}

ByteVector AadBytes(std::string_view aad) {
  return ByteVector(aad.begin(), aad.end());
}

} // namespace

ByteVector SealedDek::NewKey() {
  EnsureSodiumInit();
  ByteVector key(kKeySize);
  randombytes_buf(key.data(), key.size());
  return key;
}

Roe<void> SealedDek::SealFile(const std::filesystem::path& file, const ByteVector& key, std::string_view aad,
                              const ByteVector& dek) {
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  auto sealed = MessageCipher::Encrypt(key, dek, AadBytes(aad), *nonce);
  if (!sealed) {
    return sealed.error();
  }
  auto bytes = EncryptedPayload::EncodeBlob(*sealed);
  if (!bytes) {
    return bytes.error();
  }
  if (auto written = AtomicFileWrite::Write(file.string(), *bytes); !written) {
    return written.error();
  }
  return {};
}

std::optional<ByteVector> SealedDek::OpenFile(const std::filesystem::path& file, const ByteVector& key,
                                              std::string_view aad) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  auto bytes = ReadBoundedBytes(in, kMaxFileBytes);
  in.close();
  if (!bytes) {
    return std::nullopt;
  }
  auto blob = EncryptedPayload::DecodeBlob(std::move(*bytes));
  if (!blob) {
    return std::nullopt;
  }
  auto dek = MessageCipher::Decrypt(key, *blob, AadBytes(aad));
  if (!dek || dek->size() != kDataEncryptionKeySize) {
    if (dek) {
      Wipe(*dek);
    }
    return std::nullopt;
  }
  return std::move(*dek);
}

Roe<void> SealedDek::Seal(ISecureBlobStore& store, const std::string& entry_name, const std::filesystem::path& file,
                          std::string_view aad, const ByteVector& dek, const SecureBlobAccess access) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  ByteVector key = NewKey();
  if (auto sealed = SealFile(file, key, aad, dek); !sealed) {
    Wipe(key);
    return sealed.error();
  }
  ByteVector entry(kEntrySize);
  entry[0] = kEntryVersion;
  std::copy(key.begin(), key.end(), entry.begin() + 1);
  Wipe(key);
  auto stored = store.Write(entry_name, entry, access);
  Wipe(entry);
  if (!stored) {
    // The file alone (or with an older entry) opens nothing: leave neither piece behind.
    Forget(store, entry_name, file);
    return stored.error();
  }
  return {};
}

Roe<SealedDek::Result> SealedDek::Open(ISecureBlobStore& store, const std::string& entry_name,
                                       const std::filesystem::path& file, std::string_view aad,
                                       const std::string& prompt) {
  Result result;
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    Forget(store, entry_name, file);
    return result;
  }
  auto looked = prompt.empty() ? store.Lookup(entry_name) : store.LookupWithPrompt(entry_name, prompt);
  if (!looked) {
    return looked.error();
  }
  switch (looked->status) {
    case SecureBlobLookup::Status::Found:
      break;
    case SecureBlobLookup::Status::Missing:
      Forget(store, entry_name, file);
      return result;
    case SecureBlobLookup::Status::Unavailable:
      result.status = Status::Unavailable;
      result.code = looked->code;
      return result;
    case SecureBlobLookup::Status::Canceled:
      result.status = Status::Canceled;
      result.code = looked->code;
      return result;
  }
  if (looked->bytes.size() != kEntrySize || looked->bytes[0] != kEntryVersion) {
    Wipe(looked->bytes);
    Forget(store, entry_name, file);
    return result;
  }
  ByteVector key(looked->bytes.begin() + 1, looked->bytes.end());
  Wipe(looked->bytes);
  auto dek = OpenFile(file, key, aad);
  Wipe(key);
  if (!dek) {
    Forget(store, entry_name, file);
    return result;
  }
  result.status = Status::Found;
  result.dek = std::move(*dek);
  return result;
}

void SealedDek::Forget(ISecureBlobStore& store, const std::string& entry_name, const std::filesystem::path& file) {
  (void)store.Delete(entry_name);
  std::error_code ec;
  std::filesystem::remove(file, ec);
}

} // namespace pbr
