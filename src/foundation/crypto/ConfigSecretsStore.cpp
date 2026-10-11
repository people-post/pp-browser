#include "foundation/crypto/ConfigSecretsStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/data/AtomicFileWrite.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <sodium.h>

#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int64_t kSchemaVersion = 1;
constexpr const char* kAadPurpose = "config-secrets";

} // namespace

ConfigSecretsStore::ConfigSecretsStore(const ProfileSecretsEngine& engine) : engine_(engine) {}

ConfigSecretsStore::~ConfigSecretsStore() {
  ClearDek();
}

void ConfigSecretsStore::SetOnReady(std::function<void()> callback) {
  on_ready_ = std::move(callback);
}

std::string ConfigSecretsStore::PathFor(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "secrets.enc").string();
}

Roe<void> ConfigSecretsStore::SetDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  {
    std::lock_guard lock(mutex_);
    if (!dek_.empty()) {
      sodium_memzero(dek_.data(), dek_.size());
    }
    dek_ = std::move(dek);
  }
  if (on_ready_) {
    on_ready_();
  }
  return {};
}

void ConfigSecretsStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

bool ConfigSecretsStore::Ready() const {
  std::lock_guard lock(mutex_);
  return dek_.size() == kDataEncryptionKeySize;
}

Roe<ByteVector> ConfigSecretsStore::Seal(const ByteVector& dek, const std::string& profile_id,
                                         const ConfigSecrets& secrets) {
  Object json;
  json.set("schema_version", kSchemaVersion);
  json.set("llm_api_key", secrets.llm_api_key);
  const std::string text = DumpJson(json);
  const ByteVector plaintext(text.begin(), text.end());
  return FileCipher::Encrypt(dek, plaintext, FileCipher::BuildAad(kAadPurpose, profile_id));
}

Roe<ConfigSecrets> ConfigSecretsStore::Open(const ByteVector& dek, const std::string& profile_id,
                                            const ByteVector& blob) {
  auto plaintext = FileCipher::Decrypt(dek, blob, FileCipher::BuildAad(kAadPurpose, profile_id));
  if (!plaintext) {
    return plaintext.error();
  }
  const auto json = TryParseObject(std::string(plaintext->begin(), plaintext->end()));
  if (!json) {
    return Error("secrets.enc is not a JSON object");
  }
  const auto version = json->getIf<int64_t>("schema_version");
  if (!version || *version != kSchemaVersion) {
    return Error("Unsupported secrets.enc schema_version");
  }
  ConfigSecrets out;
  out.llm_api_key = json->getString("llm_api_key").value_or("");
  return out;
}

Roe<ConfigSecrets> ConfigSecretsStore::Load() const {
  ByteVector dek;
  {
    std::lock_guard lock(mutex_);
    if (dek_.size() != kDataEncryptionKeySize) {
      return Error("secrets.enc: profile vault is locked");
    }
    dek = dek_;
  }
  const std::string path = PathFor(engine_.ProfileDataDir());
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return ConfigSecrets{};
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxProfileJsonFileBytes) {
    return Error("secrets.enc unreadable or over the size limit");
  }
  std::ifstream in(path, std::ios::binary);
  ByteVector blob(static_cast<size_t>(size));
  if (!blob.empty() && !in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()))) {
    return Error("Failed to read secrets.enc");
  }
  auto opened = Open(dek, engine_.ProfileId(), blob);
  sodium_memzero(dek.data(), dek.size());
  return opened;
}

Roe<void> ConfigSecretsStore::Store(const ConfigSecrets& secrets) {
  ByteVector dek;
  {
    std::lock_guard lock(mutex_);
    if (dek_.size() != kDataEncryptionKeySize) {
      return Error("secrets.enc: profile vault is locked");
    }
    dek = dek_;
  }
  const std::string path = PathFor(engine_.ProfileDataDir());
  if (secrets.Empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    sodium_memzero(dek.data(), dek.size());
    return {};
  }
  auto blob = Seal(dek, engine_.ProfileId(), secrets);
  sodium_memzero(dek.data(), dek.size());
  if (!blob) {
    return blob.error();
  }
  return AtomicFileWrite::Write(path, *blob);
}

} // namespace pbr
