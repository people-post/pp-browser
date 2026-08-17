#include "base/people/IdentityStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/HybridKem.h"
#include "base/crypto/MlDsa.h"
#include "base/data/AtomicFileWrite.h"
#include "base/data/SchemaVersion.h"
#include "base/error/AppError.h"
#include "base/p2p/PeerIdUtil.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sodium.h>

namespace pbr {

namespace {

Roe<void> EnsureHybridKemKeys(LocalIdentity& identity, bool& dirty_flag) {
  const bool have_both = !identity.kem_public_key_b64.empty() && !identity.kem_private_key_b64.empty();
  if (have_both) {
    auto pk = Base64Decode(identity.kem_public_key_b64);
    auto sk = Base64Decode(identity.kem_private_key_b64);
    if (pk && sk && pk->size() == kHybridKemPublicKeyBytes && sk->size() == kHybridKemPrivateKeyBytes) {
      return {};
    }
    // Pre-release: drop legacy X25519+Kyber-draft blobs and mint ML-KEM-768.
  }
  // First create (or size mismatch): mint account KEM. Link-device Import replaces these
  // with the shared account secret (M015) after LoadOrCreate.
  auto generated = HybridKem::GenerateKeyPair();
  if (!generated) {
    return generated.error();
  }
  identity.kem_public_key_b64 = Base64Encode(generated->public_key);
  identity.kem_private_key_b64 = Base64Encode(generated->private_key);
  dirty_flag = true;
  return {};
}

Roe<void> EnsureAccountMlDsaKeys(LocalIdentity& identity, bool& dirty_flag) {
  const bool have_both =
      !identity.account_signing_public_key_b64.empty() && !identity.account_signing_private_key_b64.empty();
  if (have_both) {
    auto pk = Base64Decode(identity.account_signing_public_key_b64);
    auto sk = Base64Decode(identity.account_signing_private_key_b64);
    if (pk && sk && pk->size() == kMlDsa65PublicKeyBytes && sk->size() == kMlDsa65SecretKeyBytes) {
      auto account_id = AccountIdFromMlDsaPublicKey(*pk);
      if (!account_id) {
        return account_id.error();
      }
      if (identity.account_id != *account_id) {
        identity.account_id = *account_id;
        dirty_flag = true;
      }
      return {};
    }
  }
  auto generated = MlDsa::GenerateKeyPair();
  if (!generated) {
    return generated.error();
  }
  identity.account_signing_public_key_b64 = Base64Encode(generated->public_key);
  identity.account_signing_private_key_b64 = Base64Encode(generated->secret_key);
  auto account_id = AccountIdFromMlDsaPublicKey(generated->public_key);
  if (!account_id) {
    return account_id.error();
  }
  identity.account_id = *account_id;
  dirty_flag = true;
  return {};
}

Roe<void> DerivePeerId(LocalIdentity& identity) {
  auto public_key = Base64Decode(identity.public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  auto derived = PeerIdFromMlDsaPublicKey(*public_key);
  if (!derived) {
    return derived.error();
  }
  identity.peer_id = *derived;
  return {};
}

LocalIdentity IdentityFromJson(const nlohmann::json& root) {
  LocalIdentity identity;
  if (root.contains("public_key_b64") && root["public_key_b64"].is_string()) {
    identity.public_key_b64 = root["public_key_b64"].get<std::string>();
  }
  if (root.contains("private_key_b64") && root["private_key_b64"].is_string()) {
    identity.private_key_b64 = root["private_key_b64"].get<std::string>();
  }
  if (root.contains("account_signing_public_key_b64") && root["account_signing_public_key_b64"].is_string()) {
    identity.account_signing_public_key_b64 = root["account_signing_public_key_b64"].get<std::string>();
  }
  if (root.contains("account_signing_private_key_b64") && root["account_signing_private_key_b64"].is_string()) {
    identity.account_signing_private_key_b64 = root["account_signing_private_key_b64"].get<std::string>();
  }
  if (root.contains("account_id") && root["account_id"].is_string()) {
    identity.account_id = root["account_id"].get<std::string>();
  }
  if (root.contains("nickname") && root["nickname"].is_string()) {
    identity.nickname = root["nickname"].get<std::string>();
  }
  if (root.contains("relay_user_id") && root["relay_user_id"].is_string()) {
    identity.relay_user_id = root["relay_user_id"].get<std::string>();
  }
  if (root.contains("brief_llm_api_key") && root["brief_llm_api_key"].is_string()) {
    identity.brief_llm_api_key = root["brief_llm_api_key"].get<std::string>();
  }
  if (root.contains("registered") && root["registered"].is_boolean()) {
    identity.registered = root["registered"].get<bool>();
  }
  if (root.contains("registration_expires_at") && root["registration_expires_at"].is_string()) {
    identity.registration_expires_at = root["registration_expires_at"].get<std::string>();
  }
  if (root.contains("kem_public_key_b64") && root["kem_public_key_b64"].is_string()) {
    identity.kem_public_key_b64 = root["kem_public_key_b64"].get<std::string>();
  }
  if (root.contains("kem_private_key_b64") && root["kem_private_key_b64"].is_string()) {
    identity.kem_private_key_b64 = root["kem_private_key_b64"].get<std::string>();
  }
  if (root.contains("initiation_floor") && root["initiation_floor"].is_number_integer()) {
    identity.initiation_floor = root["initiation_floor"].get<int64_t>();
  }
  return identity;
}

nlohmann::json IdentityToJson(const LocalIdentity& identity) {
  return {{"schema_version", IdentityStore::kSchemaVersion},
          {"public_key_b64", identity.public_key_b64},
          {"private_key_b64", identity.private_key_b64},
          {"account_signing_public_key_b64", identity.account_signing_public_key_b64},
          {"account_signing_private_key_b64", identity.account_signing_private_key_b64},
          {"account_id", identity.account_id},
          {"kem_public_key_b64", identity.kem_public_key_b64},
          {"kem_private_key_b64", identity.kem_private_key_b64},
          {"nickname", identity.nickname},
          {"relay_user_id", identity.relay_user_id},
          {"brief_llm_api_key", identity.brief_llm_api_key},
          {"registered", identity.registered},
          {"registration_expires_at", identity.registration_expires_at},
          {"initiation_floor", identity.initiation_floor}};
}

} // namespace

IdentityStore::IdentityStore(std::string data_dir, std::string profile_id)
    : data_dir_(std::move(data_dir)), profile_id_(std::move(profile_id)) {
  redirectLogger("IdentityStore");
  if (profile_id_.empty()) {
    profile_id_ = std::filesystem::path(data_dir_).filename().string();
    if (profile_id_.empty()) {
      profile_id_ = "default";
    }
  }
}

Roe<void> IdentityStore::SetDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
  }
  dek_ = std::move(dek);
  loaded_ = false;
  return {};
}

Roe<void> IdentityStore::ReplaceDekKeepLoaded(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
  }
  dek_ = std::move(dek);
  return {};
}

void IdentityStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
  loaded_ = false;
}

Roe<void> IdentityStore::RequireDek() const {
  if (dek_.size() != kDataEncryptionKeySize) {
    return AppError::Pin(Err::Pin::Required, "IdentityStore DEK not set (unlock profile vault first)");
  }
  return {};
}

std::string IdentityStore::StorePath() const {
  return (std::filesystem::path(data_dir_) / "identity.enc").string();
}

std::string IdentityStore::ProfileId() const {
  return profile_id_;
}

Roe<void> IdentityStore::EnsureLoaded() const {
  if (loaded_) {
    return {};
  }
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }

  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  std::ifstream in(StorePath(), std::ios::binary);
  if (!in) {
    auto keys = MlDsa::GenerateKeyPair();
    if (!keys) {
      return keys.error();
    }
    private_key_ = keys->secret_key;
    identity_.public_key_b64 = Base64Encode(keys->public_key);
    identity_.private_key_b64 = Base64Encode(keys->secret_key);
    identity_.nickname = "user";
    identity_.relay_user_id.clear();
    identity_.brief_llm_api_key.clear();
    identity_.registered = false;
    identity_.registration_expires_at.clear();
    if (auto peer = DerivePeerId(identity_); !peer) {
      return peer.error();
    }
    if (auto kem = EnsureHybridKemKeys(identity_, dirty_); !kem) {
      return kem.error();
    }
    if (auto account = EnsureAccountMlDsaKeys(identity_, dirty_); !account) {
      return account.error();
    }
    loaded_ = true;
    dirty_ = true;
    return {};
  }

  ByteVector blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string aad = FileCipher::BuildAad("identity", ProfileId());
  auto plaintext = FileCipher::Decrypt(dek_, blob, aad);
  if (!plaintext) {
    return plaintext.error();
  }
  const nlohmann::json root =
      nlohmann::json::parse(plaintext->begin(), plaintext->end(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("Failed to parse decrypted identity");
  }

  int version = 0;
  if (root.contains("schema_version")) {
    if (!root["schema_version"].is_number_integer()) {
      return Error("Invalid schema_version in identity.enc");
    }
    version = root["schema_version"].get<int>();
    if (auto checked = SchemaVersion::Validate(root, kSchemaVersion, "identity.enc"); !checked) {
      return checked.error();
    }
  }
  if (version < kSchemaVersion) {
    return Error(
        "identity.enc schema is pre-PQ device key (Ed25519); wipe profile data and recreate "
        "(libp2p-pq-transport hard cut)");
  }

  identity_ = IdentityFromJson(root);
  auto private_key = Base64Decode(identity_.private_key_b64);
  if (!private_key) {
    return private_key.error();
  }
  if (private_key->size() != kMlDsa65SecretKeyBytes) {
    return Error(
        "identity.enc device key is not ML-DSA-65; wipe profile data and recreate "
        "(libp2p-pq-transport hard cut)");
  }
  private_key_ = std::move(*private_key);

  if (auto peer = DerivePeerId(identity_); !peer) {
    return peer.error();
  }
  if (auto kem = EnsureHybridKemKeys(identity_, dirty_); !kem) {
    return kem.error();
  }
  if (auto account = EnsureAccountMlDsaKeys(identity_, dirty_); !account) {
    return account.error();
  }
  loaded_ = true;
  return {};
}

Roe<void> IdentityStore::Save() const {
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  const std::string json = IdentityToJson(identity_).dump(2);
  const ByteVector plaintext(json.begin(), json.end());
  const std::string aad = FileCipher::BuildAad("identity", ProfileId());
  auto ciphertext = FileCipher::Encrypt(dek_, plaintext, aad);
  if (!ciphertext) {
    return ciphertext.error();
  }
  return AtomicFileWrite::Write(StorePath(), *ciphertext);
}

void IdentityStore::Flush() {
  std::lock_guard lock(mutex_);
  if (!dirty_) {
    return;
  }
  if (Save()) {
    dirty_ = false;
  }
}

Roe<LocalIdentity> IdentityStore::LoadOrCreate() {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  if (dirty_ && Save()) {
    dirty_ = false;
  }
  return identity_;
}

Roe<LocalIdentity> IdentityStore::Get() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  return identity_;
}

Roe<LocalIdentity> IdentityStore::Update(const LocalIdentity& identity) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  identity_ = identity;
  if (auto peer = DerivePeerId(identity_); !peer) {
    return peer.error();
  }
  dirty_ = true;
  if (Save()) {
    dirty_ = false;
    return identity_;
  }
  return Error("Failed to save identity");
}

Roe<std::string> IdentityStore::SignPayload(const std::string& canonical_json) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const ByteVector message(canonical_json.begin(), canonical_json.end());
  auto secret = Base64Decode(identity_.account_signing_private_key_b64);
  if (!secret) {
    return secret.error();
  }
  auto signature = MlDsa::Sign(*secret, message);
  if (!signature) {
    return signature.error();
  }
  return Base64Encode(*signature);
}

Roe<std::string> IdentityStore::SignBytes(const std::vector<uint8_t>& sign_bytes) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  auto secret = Base64Decode(identity_.account_signing_private_key_b64);
  if (!secret) {
    return secret.error();
  }
  if (secret->size() != kMlDsa65SecretKeyBytes) {
    return Error("Invalid ML-DSA-65 secret key size");
  }
  auto signature = MlDsa::Sign(*secret, sign_bytes);
  if (!signature) {
    return signature.error();
  }
  return Base64Encode(*signature);
}

Roe<ByteVector> IdentityStore::GetDeviceMlDsaPrivateKey() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  if (private_key_.size() != kMlDsa65SecretKeyBytes) {
    return Error("Invalid device ML-DSA-65 private key size");
  }
  return private_key_;
}

Roe<ByteVector> IdentityStore::GetDeviceMlDsaPublicKey() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  auto public_key = Base64Decode(identity_.public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  if (public_key->size() != kMlDsa65PublicKeyBytes) {
    return Error("Invalid device ML-DSA-65 public key size");
  }
  return *public_key;
}

Roe<ByteVector> IdentityStore::GetOrCreateHybridKemPrivateKey() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  auto decoded = Base64Decode(identity_.kem_private_key_b64);
  if (!decoded) {
    return decoded.error();
  }
  if (decoded->size() != kHybridKemPrivateKeyBytes) {
    return Error("Invalid hybrid KEM private key size");
  }
  return *decoded;
}

Roe<std::string> IdentityStore::GetHybridKemPublicKeyB64() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  if (identity_.kem_public_key_b64.empty()) {
    return Error("Hybrid KEM public key missing");
  }
  return identity_.kem_public_key_b64;
}

Roe<ByteVector> IdentityStore::GetAccountMlDsaPrivateKey() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  auto decoded = Base64Decode(identity_.account_signing_private_key_b64);
  if (!decoded) {
    return decoded.error();
  }
  if (decoded->size() != kMlDsa65SecretKeyBytes) {
    return Error("Invalid ML-DSA-65 secret key size");
  }
  return *decoded;
}

Roe<std::string> IdentityStore::GetAccountId() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  if (identity_.account_id.empty()) {
    return Error("Account ID missing");
  }
  return identity_.account_id;
}

} // namespace pbr
