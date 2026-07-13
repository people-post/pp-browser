#include "base/crypto/DataKeyVault.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/EncryptedPayload.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/MessageCipher.h"
#include "base/data/AtomicFileWrite.h"
#include "base/error/AppError.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sodium.h>

namespace pbr {

namespace {

constexpr char kVaultMagic[4] = {'P', 'P', 'B', 'V'};
constexpr size_t kVaultHeaderSize = 4 + 1 + 8 + 8 + crypto_pwhash_SALTBYTES + kAeadNonceSize;

void AppendU64Le(ByteVector& out, const uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xffu));
  }
}

Roe<uint64_t> ReadU64Le(const ByteVector& bytes, const size_t offset) {
  if (bytes.size() < offset + 8) {
    return Error("Vault file truncated");
  }
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(bytes[offset + static_cast<size_t>(i)]) << (8 * i);
  }
  return value;
}

} // namespace

DataKeyVault::DataKeyVault(std::string vault_path, std::string profile_id)
    : vault_path_(std::move(vault_path)), profile_id_(std::move(profile_id)) {}

std::string DataKeyVault::VaultPathForProfile(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "vault.bin").string();
}

bool DataKeyVault::Exists(const std::string& vault_path) {
  std::error_code ec;
  return std::filesystem::exists(vault_path, ec) && !ec;
}

bool DataKeyVault::Exists() const {
  return Exists(vault_path_);
}

void DataKeyVault::Lock() {
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<ByteVector> DataKeyVault::Dek() const {
  if (dek_.empty()) {
    return AppError::Pin(Err::Pin::Required, "Profile vault is locked");
  }
  return dek_;
}

Roe<void> DataKeyVault::UnlockWithDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  Lock();
  dek_ = std::move(dek);
  return {};
}

Roe<ByteVector> DataKeyVault::WrapDek(const ByteVector& kek, const ByteVector& dek) const {
  const std::string aad = FileCipher::BuildAad("vault-dek", profile_id_);
  return FileCipher::Encrypt(kek, dek, aad);
}

Roe<ByteVector> DataKeyVault::UnwrapDek(const ByteVector& kek, const ByteVector& wrapped) const {
  const std::string aad = FileCipher::BuildAad("vault-dek", profile_id_);
  return FileCipher::Decrypt(kek, wrapped, aad);
}

Roe<ByteVector> DataKeyVault::ReadVaultFile() const {
  std::ifstream in(vault_path_, std::ios::binary);
  if (!in) {
    return Error("Failed to open vault.bin");
  }
  ByteVector bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < kVaultHeaderSize + crypto_aead_xchacha20poly1305_ietf_abytes()) {
    return Error("vault.bin too short");
  }
  if (std::memcmp(bytes.data(), kVaultMagic, 4) != 0) {
    return Error("Invalid vault.bin magic");
  }
  if (bytes[4] != kVaultFileVersion) {
    return Error("Unsupported vault.bin version");
  }
  return bytes;
}

Roe<void> DataKeyVault::WriteVaultFile(const PinKdfParams& params, const ByteVector& wrapped_dek) const {
  if (params.salt.size() != crypto_pwhash_SALTBYTES) {
    return Error("Invalid vault salt");
  }
  auto decoded = EncryptedPayload::DecodeBlob(wrapped_dek);
  if (!decoded) {
    return decoded.error();
  }
  ByteVector out;
  out.reserve(kVaultHeaderSize + decoded->ciphertext.size());
  out.insert(out.end(), kVaultMagic, kVaultMagic + 4);
  out.push_back(kVaultFileVersion);
  AppendU64Le(out, params.opslimit);
  AppendU64Le(out, params.memlimit);
  out.insert(out.end(), params.salt.begin(), params.salt.end());
  out.insert(out.end(), decoded->nonce.begin(), decoded->nonce.end());
  out.insert(out.end(), decoded->ciphertext.begin(), decoded->ciphertext.end());
  return AtomicFileWrite::Write(vault_path_, out);
}

Roe<void> DataKeyVault::Create(std::string_view pin) {
  if (Exists()) {
    return Error("vault.bin already exists");
  }
  auto params = PinKeyDeriver::GenerateParams();
  if (!params) {
    return params.error();
  }
  auto kek = PinKeyDeriver::DeriveKek(pin, *params);
  if (!kek) {
    return kek.error();
  }
  EnsureSodiumInit();
  ByteVector dek(kDataEncryptionKeySize);
  randombytes_buf(dek.data(), dek.size());
  auto wrapped = WrapDek(*kek, dek);
  sodium_memzero(kek->data(), kek->size());
  if (!wrapped) {
    sodium_memzero(dek.data(), dek.size());
    return wrapped.error();
  }
  if (auto written = WriteVaultFile(*params, *wrapped); !written) {
    sodium_memzero(dek.data(), dek.size());
    return written.error();
  }
  Lock();
  dek_ = std::move(dek);
  return {};
}

Roe<void> DataKeyVault::Unlock(std::string_view pin) {
  auto bytes = ReadVaultFile();
  if (!bytes) {
    return bytes.error();
  }
  auto opslimit = ReadU64Le(*bytes, 5);
  if (!opslimit) {
    return opslimit.error();
  }
  auto memlimit = ReadU64Le(*bytes, 13);
  if (!memlimit) {
    return memlimit.error();
  }
  PinKdfParams params;
  params.opslimit = *opslimit;
  params.memlimit = *memlimit;
  params.salt.assign(bytes->begin() + 21, bytes->begin() + 21 + crypto_pwhash_SALTBYTES);
  const size_t nonce_off = 21 + crypto_pwhash_SALTBYTES;
  EncryptedBlob wrapped_blob;
  wrapped_blob.nonce.assign(bytes->begin() + nonce_off, bytes->begin() + nonce_off + kAeadNonceSize);
  wrapped_blob.ciphertext.assign(bytes->begin() + nonce_off + kAeadNonceSize, bytes->end());
  auto wrapped = EncryptedPayload::EncodeBlob(wrapped_blob);
  if (!wrapped) {
    return wrapped.error();
  }
  auto kek = PinKeyDeriver::DeriveKek(pin, params);
  if (!kek) {
    return kek.error();
  }
  auto dek = UnwrapDek(*kek, *wrapped);
  sodium_memzero(kek->data(), kek->size());
  if (!dek) {
    return AppError::Pin(Err::Pin::Generic, "Incorrect PIN or corrupt vault")
        .WithUser("Incorrect PIN or corrupt vault");
  }
  Lock();
  dek_ = std::move(*dek);
  return {};
}

Roe<void> DataKeyVault::ChangePin(std::string_view old_pin, std::string_view new_pin) {
  if (auto unlocked = Unlock(old_pin); !unlocked) {
    return unlocked.error();
  }
  ByteVector dek = dek_;
  auto params = PinKeyDeriver::GenerateParams();
  if (!params) {
    return params.error();
  }
  auto kek = PinKeyDeriver::DeriveKek(new_pin, *params);
  if (!kek) {
    return kek.error();
  }
  auto wrapped = WrapDek(*kek, dek);
  sodium_memzero(kek->data(), kek->size());
  if (!wrapped) {
    return wrapped.error();
  }
  if (auto written = WriteVaultFile(*params, *wrapped); !written) {
    return written.error();
  }
  Lock();
  dek_ = std::move(dek);
  return {};
}

} // namespace pbr
