#include "base/people/IdentityStore.h"

#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"
#include "base/people/Ed25519Signer.h"
#include "libp2p/integration/host/PeerIdUtil.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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

} // namespace

IdentityStore::IdentityStore(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("IdentityStore");
}

std::string IdentityStore::StorePath() const {
  return (std::filesystem::path(data_dir_) / "identity.json").string();
}

Roe<void> IdentityStore::EnsureLoaded() const {
  if (loaded_) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  std::ifstream in(StorePath());
  if (!in) {
    auto keys = Ed25519Signer::GenerateKeyPair();
    if (!keys) {
      return keys.error();
    }
    private_key_ = keys->private_key;
    identity_.public_key_b64 = Ed25519Signer::ToBase64(keys->public_key);
    identity_.encrypted_private_key_b64 = Ed25519Signer::ToBase64(keys->private_key);
    identity_.nickname = "user";
    identity_.relay_user_id.clear();
    identity_.registered = false;
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

  const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Error("Failed to parse identity.json");
  }

  if (root.contains("public_key_b64") && root["public_key_b64"].is_string()) {
    identity_.public_key_b64 = root["public_key_b64"].get<std::string>();
  }
  if (root.contains("encrypted_private_key_b64") && root["encrypted_private_key_b64"].is_string()) {
    identity_.encrypted_private_key_b64 = root["encrypted_private_key_b64"].get<std::string>();
  }
  if (root.contains("nickname") && root["nickname"].is_string()) {
    identity_.nickname = root["nickname"].get<std::string>();
  }
  if (root.contains("relay_user_id") && root["relay_user_id"].is_string()) {
    identity_.relay_user_id = root["relay_user_id"].get<std::string>();
  }
  if (root.contains("registered") && root["registered"].is_boolean()) {
    identity_.registered = root["registered"].get<bool>();
  }
  if (root.contains("kem_public_key_b64") && root["kem_public_key_b64"].is_string()) {
    identity_.kem_public_key_b64 = root["kem_public_key_b64"].get<std::string>();
  }
  if (root.contains("kem_private_key_b64") && root["kem_private_key_b64"].is_string()) {
    identity_.kem_private_key_b64 = root["kem_private_key_b64"].get<std::string>();
  }

  auto private_key = Ed25519Signer::FromBase64(identity_.encrypted_private_key_b64);
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
  return {};
}

Roe<void> IdentityStore::Save() const {
  const nlohmann::json root = {{"public_key_b64", identity_.public_key_b64},
                               {"encrypted_private_key_b64", identity_.encrypted_private_key_b64},
                               {"kem_public_key_b64", identity_.kem_public_key_b64},
                               {"kem_private_key_b64", identity_.kem_private_key_b64},
                               {"nickname", identity_.nickname},
                               {"relay_user_id", identity_.relay_user_id},
                               {"registered", identity_.registered}};

  std::ofstream out(StorePath());
  if (!out) {
    return Error("Failed to write identity.json");
  }
  out << root.dump(2);
  return {};
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
