#include "base/people/IdentityStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/HybridKem.h"
#include "base/data/AtomicFileWrite.h"
#include "base/data/SchemaVersion.h"
#include "base/error/AppError.h"
#include "base/people/Ed25519Signer.h"
#include "libp2p/integration/host/PeerIdUtil.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sodium.h>

namespace pbr {

namespace {

Roe<void> EnsureHybridKemKeys(LocalIdentity& identity, bool& dirty_flag) {
  if (!identity.kem_public_key_b64.empty() && !identity.kem_private_key_b64.empty()) {
    return {};
  }
  auto generated = HybridKem::GenerateKeyPair();
  if (!generated) {
    return generated.error();
  }
  identity.kem_public_key_b64 = Base64Encode(generated->public_key);
  identity.kem_private_key_b64 = Base64Encode(generated->private_key);
  dirty_flag = true;
  return {};
}

Roe<void> DerivePeerId(LocalIdentity& identity) {
  auto public_key = Ed25519Signer::FromBase64(identity.public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  auto derived = PeerIdFromEd25519PublicKey(*public_key);
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
  return identity;
}

nlohmann::json IdentityToJson(const LocalIdentity& identity) {
  return {{"schema_version", IdentityStore::kSchemaVersion},
          {"public_key_b64", identity.public_key_b64},
          {"private_key_b64", identity.private_key_b64},
          {"kem_public_key_b64", identity.kem_public_key_b64},
          {"kem_private_key_b64", identity.kem_private_key_b64},
          {"nickname", identity.nickname},
          {"relay_user_id", identity.relay_user_id},
          {"brief_llm_api_key", identity.brief_llm_api_key},
          {"registered", identity.registered},
          {"registration_expires_at", identity.registration_expires_at}};
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
    auto keys = Ed25519Signer::GenerateKeyPair();
    if (!keys) {
      return keys.error();
    }
    private_key_ = keys->private_key;
    identity_.public_key_b64 = Ed25519Signer::ToBase64(keys->public_key);
    identity_.private_key_b64 = Ed25519Signer::ToBase64(keys->private_key);
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

  identity_ = IdentityFromJson(root);
  auto private_key = Ed25519Signer::FromBase64(identity_.private_key_b64);
  if (!private_key) {
    return private_key.error();
  }
  private_key_ = std::move(*private_key);

  if (auto peer = DerivePeerId(identity_); !peer) {
    return peer.error();
  }
  if (auto kem = EnsureHybridKemKeys(identity_, dirty_); !kem) {
    return kem.error();
  }
  loaded_ = true;
  if (version < kSchemaVersion) {
    dirty_ = true;
  }
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
  return Ed25519Signer::Sign(canonical_json, private_key_);
}

Roe<std::string> IdentityStore::SignBytes(const std::vector<uint8_t>& sign_bytes) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  return Ed25519Signer::Sign(std::string(sign_bytes.begin(), sign_bytes.end()), private_key_);
}

Roe<ByteVector> IdentityStore::GetEd25519PrivateKey() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  if (private_key_.size() != 32) {
    return Error("Invalid Ed25519 private key size");
  }
  return private_key_;
}

Roe<ByteVector> IdentityStore::GetEd25519PublicKey() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  auto public_key = Ed25519Signer::FromBase64(identity_.public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  if (public_key->size() != 32) {
    return Error("Invalid Ed25519 public key size");
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

} // namespace pbr
