#include "base/people/IdentityStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/HybridKem.h"
#include "base/crypto/IdentitySeedDeriver.h"
#include "base/crypto/MlDsa.h"
#include "base/data/AtomicFileWrite.h"
#include "base/data/SchemaVersion.h"
#include "foundation/error/AppError.h"
#include "base/mesh/identity/PeerIdUtil.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<void> EnsureHybridKemKeys(LocalIdentity& identity, bool& dirty_flag,
                              const ByteVector* seeded_coins = nullptr) {
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
  Roe<HybridKemKeyPair> generated =
      seeded_coins ? HybridKem::GenerateKeyPairFromSeed(*seeded_coins) : HybridKem::GenerateKeyPair();
  if (!generated) {
    return generated.error();
  }
  identity.kem_public_key_b64 = Base64Encode(generated->public_key);
  identity.kem_private_key_b64 = Base64Encode(generated->private_key);
  dirty_flag = true;
  return {};
}

Roe<void> EnsureAccountMlDsaKeys(LocalIdentity& identity, bool& dirty_flag,
                                 const ByteVector* seeded_seed = nullptr) {
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
  Roe<MlDsaKeyPair> generated =
      seeded_seed ? MlDsa::GenerateKeyPairFromSeed(*seeded_seed) : MlDsa::GenerateKeyPair();
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

bool ConstantTimeEqualB64(const std::string& a, const std::string& b) {
  auto da = Base64Decode(a);
  auto db = Base64Decode(b);
  if (!da || !db || da->size() != db->size()) {
    return false;
  }
  return sodium_memcmp(da->data(), db->data(), da->size()) == 0;
}

Roe<void> FailClosedIfSeedMismatch(const LocalIdentity& identity, const DerivedNodeIdentitySeeds& seeds) {
  auto device = MlDsa::GenerateKeyPairFromSeed(seeds.device_ml_dsa_seed);
  if (!device) {
    return device.error();
  }
  auto account = MlDsa::GenerateKeyPairFromSeed(seeds.account_ml_dsa_seed);
  if (!account) {
    return account.error();
  }
  auto kem = HybridKem::GenerateKeyPairFromSeed(seeds.account_ml_kem_coins);
  if (!kem) {
    return kem.error();
  }
  const std::string expect_device_pk = Base64Encode(device->public_key);
  const std::string expect_account_pk = Base64Encode(account->public_key);
  const std::string expect_kem_pk = Base64Encode(kem->public_key);
  if (!ConstantTimeEqualB64(identity.public_key_b64, expect_device_pk) ||
      !ConstantTimeEqualB64(identity.account_signing_public_key_b64, expect_account_pk) ||
      !ConstantTimeEqualB64(identity.kem_public_key_b64, expect_kem_pk)) {
    return Error(
        "PP_NODE_IDENTITY_SEED does not match existing identity.enc (fail-closed); "
        "use the seed that minted this profile, or wipe the volume");
  }
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

LocalIdentity IdentityFromJson(const Object& root) {
  LocalIdentity identity;
  if (auto v = root.getString("public_key_b64")) {
    identity.public_key_b64 = *v;
  }
  if (auto v = root.getString("private_key_b64")) {
    identity.private_key_b64 = *v;
  }
  if (auto v = root.getString("account_signing_public_key_b64")) {
    identity.account_signing_public_key_b64 = *v;
  }
  if (auto v = root.getString("account_signing_private_key_b64")) {
    identity.account_signing_private_key_b64 = *v;
  }
  if (auto v = root.getString("account_id")) {
    identity.account_id = *v;
  }
  if (auto v = root.getString("nickname")) {
    identity.nickname = *v;
  }
  if (auto v = root.getString("relay_user_id")) {
    identity.relay_user_id = *v;
  }
  if (auto v = root.getString("brief_llm_api_key")) {
    identity.brief_llm_api_key = *v;
  }
  if (auto v = root.getString("brief_llm_guest_api_key")) {
    identity.brief_llm_guest_api_key = *v;
  }
  if (auto v = root.getIf<bool>("registered")) {
    identity.registered = *v;
  }
  if (auto v = root.getString("registration_expires_at")) {
    identity.registration_expires_at = *v;
  }
  if (auto v = root.getString("kem_public_key_b64")) {
    identity.kem_public_key_b64 = *v;
  }
  if (auto v = root.getString("kem_private_key_b64")) {
    identity.kem_private_key_b64 = *v;
  }
  if (auto v = root.getIf<int64_t>("initiation_floor")) {
    identity.initiation_floor = *v;
  }
  return identity;
}

Object IdentityToJson(const LocalIdentity& identity) {
  Object root;
  root.set("schema_version", static_cast<int64_t>(IdentityStore::kSchemaVersion));
  root.set("public_key_b64", identity.public_key_b64);
  root.set("private_key_b64", identity.private_key_b64);
  root.set("account_signing_public_key_b64", identity.account_signing_public_key_b64);
  root.set("account_signing_private_key_b64", identity.account_signing_private_key_b64);
  root.set("account_id", identity.account_id);
  root.set("kem_public_key_b64", identity.kem_public_key_b64);
  root.set("kem_private_key_b64", identity.kem_private_key_b64);
  root.set("nickname", identity.nickname);
  root.set("relay_user_id", identity.relay_user_id);
  root.set("brief_llm_api_key", identity.brief_llm_api_key);
  root.set("brief_llm_guest_api_key", identity.brief_llm_guest_api_key);
  root.set("registered", identity.registered);
  root.set("registration_expires_at", identity.registration_expires_at);
  root.set("initiation_floor", identity.initiation_floor);
  return root;
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

Roe<void> IdentityStore::SetIdentitySeed(ByteVector master_seed) {
  if (master_seed.size() < 32) {
    return Error("identity seed must be at least 32 bytes");
  }
  std::lock_guard lock(mutex_);
  if (!identity_seed_.empty()) {
    sodium_memzero(identity_seed_.data(), identity_seed_.size());
  }
  identity_seed_ = std::move(master_seed);
  return {};
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
    std::optional<DerivedNodeIdentitySeeds> derived;
    if (!identity_seed_.empty()) {
      auto seeds = DeriveNodeIdentitySeeds(identity_seed_);
      if (!seeds) {
        return seeds.error();
      }
      derived = std::move(*seeds);
    }
    Roe<MlDsaKeyPair> keys =
        derived ? MlDsa::GenerateKeyPairFromSeed(derived->device_ml_dsa_seed) : MlDsa::GenerateKeyPair();
    if (!keys) {
      return keys.error();
    }
    private_key_ = keys->secret_key;
    identity_.public_key_b64 = Base64Encode(keys->public_key);
    identity_.private_key_b64 = Base64Encode(keys->secret_key);
    identity_.nickname = "user";
    identity_.relay_user_id.clear();
    identity_.brief_llm_api_key.clear();
    identity_.brief_llm_guest_api_key.clear();
    identity_.registered = false;
    identity_.registration_expires_at.clear();
    if (auto peer = DerivePeerId(identity_); !peer) {
      return peer.error();
    }
    const ByteVector* kem_coins = derived ? &derived->account_ml_kem_coins : nullptr;
    const ByteVector* account_seed = derived ? &derived->account_ml_dsa_seed : nullptr;
    if (auto kem = EnsureHybridKemKeys(identity_, dirty_, kem_coins); !kem) {
      return kem.error();
    }
    if (auto account = EnsureAccountMlDsaKeys(identity_, dirty_, account_seed); !account) {
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
  const std::string text(plaintext->begin(), plaintext->end());
  auto root = TryParseObject(text);
  if (!root) {
    return Error("Failed to parse decrypted identity");
  }

  int version = 0;
  if (root->contains("schema_version")) {
    auto version_opt = root->getIf<int64_t>("schema_version");
    if (!version_opt) {
      if (auto as_u = root->getNonNegInt("schema_version")) {
        version_opt = static_cast<int64_t>(*as_u);
      }
    }
    if (!version_opt) {
      return Error("Invalid schema_version in identity.enc");
    }
    version = static_cast<int>(*version_opt);
    if (auto checked = SchemaVersion::Validate(*root, kSchemaVersion, "identity.enc"); !checked) {
      return checked.error();
    }
  }
  if (version < kSchemaVersion) {
    return Error(
        "identity.enc schema is pre-PQ device key (Ed25519); wipe profile data and recreate "
        "(libp2p-pq-transport hard cut)");
  }

  identity_ = IdentityFromJson(*root);
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
  if (!identity_seed_.empty()) {
    auto seeds = DeriveNodeIdentitySeeds(identity_seed_);
    if (!seeds) {
      return seeds.error();
    }
    if (auto matched = FailClosedIfSeedMismatch(identity_, *seeds); !matched) {
      return matched.error();
    }
  }
  loaded_ = true;
  return {};
}

Roe<void> IdentityStore::Save() const {
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  const std::string json = DumpJson(IdentityToJson(identity_), 2);
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
