#include "foundation/crypto/LinkDeviceCodec.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoverySeed.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char* kLinkDeviceFormat = "pp-browser-link-device-v1";

Roe<void> RequireB64Size(const std::string& b64, const size_t expected, const char* label) {
  auto bytes = Base64Decode(b64);
  if (!bytes) {
    return Error(std::string(label) + " is not valid base64");
  }
  if (bytes->size() != expected) {
    return Error(std::string(label) + " has wrong size");
  }
  return {};
}

} // namespace

Roe<void> LinkDeviceCodec::Validate(const LinkDeviceBundleV1& bundle, const int64_t now_ms) {
  if (bundle.account_id.rfind("account:", 0) != 0) {
    return Error("Link bundle missing Account ID");
  }
  if (bundle.relay_user_id.rfind("relay:", 0) != 0) {
    return Error("Link bundle missing relay user id");
  }
  if (bundle.created_at_ms <= 0 || bundle.expires_at_ms <= bundle.created_at_ms) {
    return Error("Link bundle has invalid expiry");
  }
  if (now_ms > 0 && now_ms >= bundle.expires_at_ms) {
    return Error("Link bundle has expired");
  }
  if (auto pk = RequireB64Size(bundle.account_ml_dsa_pk_b64, kMlDsa65PublicKeyBytes, "Account public key"); !pk) {
    return pk.error();
  }
  if (auto sk = RequireB64Size(bundle.account_ml_dsa_sk_b64, kMlDsa65SecretKeyBytes, "Account secret key"); !sk) {
    return sk.error();
  }
  if (auto kem_pk = RequireB64Size(bundle.account_kem_pk_b64, kHybridKemPublicKeyBytes, "Account KEM public key");
      !kem_pk) {
    return kem_pk.error();
  }
  if (auto kem_sk = RequireB64Size(bundle.account_kem_sk_b64, kHybridKemPrivateKeyBytes, "Account KEM secret key");
      !kem_sk) {
    return kem_sk.error();
  }
  if (auto dek = RequireB64Size(bundle.dek_b64, kDataEncryptionKeySize, "DEK"); !dek) {
    return dek.error();
  }
  auto pk_bytes = Base64Decode(bundle.account_ml_dsa_pk_b64);
  if (!pk_bytes) {
    return pk_bytes.error();
  }
  auto derived = AccountIdFromMlDsaPublicKey(*pk_bytes);
  if (!derived) {
    return derived.error();
  }
  if (*derived != bundle.account_id) {
    return Error("Link bundle Account ID does not match signing key");
  }
  if (bundle.recovery_seed_b64) {
    // A seed that derives other keys would show a phrase that restores another account: refuse it.
    auto seed = Base64Decode(*bundle.recovery_seed_b64);
    auto keys = seed ? DeriveRecoveryKeys(*seed) : Roe<RecoveryKeys>(seed.error());
    auto signing = keys ? MlDsa::GenerateKeyPairFromSeed(keys->account_ml_dsa_seed) : Roe<MlDsaKeyPair>(keys.error());
    auto kem = keys ? HybridKem::GenerateKeyPairFromSeed(keys->account_ml_kem_coins) : Roe<HybridKemKeyPair>(keys.error());
    if (!signing || !kem || Base64Encode(signing->public_key) != bundle.account_ml_dsa_pk_b64 ||
        Base64Encode(kem->public_key) != bundle.account_kem_pk_b64) {
      return Error("Link bundle recovery seed does not derive its account keys");
    }
  }
  return {};
}

Roe<std::string> LinkDeviceCodec::Serialize(const LinkDeviceBundleV1& bundle) {
  if (auto valid = Validate(bundle); !valid) {
    return valid.error();
  }
  Object json;
  json.set("format", kLinkDeviceFormat);
  json.set("account_id", bundle.account_id);
  json.set("account_ml_dsa_pk_b64", bundle.account_ml_dsa_pk_b64);
  json.set("account_ml_dsa_sk_b64", bundle.account_ml_dsa_sk_b64);
  json.set("account_kem_pk_b64", bundle.account_kem_pk_b64);
  json.set("account_kem_sk_b64", bundle.account_kem_sk_b64);
  json.set("dek_b64", bundle.dek_b64);
  json.set("relay_user_id", bundle.relay_user_id);
  json.set("nickname", bundle.nickname);
  json.set("created_at", bundle.created_at_ms);
  json.set("expires_at", bundle.expires_at_ms);
  if (bundle.recovery_seed_b64) {
    json.set("recovery_seed_b64", *bundle.recovery_seed_b64);
  }
  if (bundle.account_card_json) {
    json.set("account_card_json", *bundle.account_card_json);
  }
  const std::string serialized = DumpJson(json);
  if (serialized.size() > kMaxLinkDeviceBundleBytes) {
    return Error("Serialized link-device bundle exceeds size limit");
  }
  return serialized;
}

Roe<LinkDeviceBundleV1> LinkDeviceCodec::Parse(const std::string& json) {
  if (json.size() > kMaxLinkDeviceBundleBytes) {
    return Error("Link-device bundle exceeds size limit");
  }
  auto parsed = TryParseObject(json);
  if (!parsed) {
    return Error("Invalid link-device bundle JSON");
  }
  if (parsed->getString("format").value_or(std::string{}) != kLinkDeviceFormat) {
    return Error("Unsupported link-device bundle format");
  }

  LinkDeviceBundleV1 bundle;
  bundle.account_id = parsed->getString("account_id").value_or(std::string{});
  bundle.account_ml_dsa_pk_b64 = parsed->getString("account_ml_dsa_pk_b64").value_or(std::string{});
  bundle.account_ml_dsa_sk_b64 = parsed->getString("account_ml_dsa_sk_b64").value_or(std::string{});
  bundle.account_kem_pk_b64 = parsed->getString("account_kem_pk_b64").value_or(std::string{});
  bundle.account_kem_sk_b64 = parsed->getString("account_kem_sk_b64").value_or(std::string{});
  bundle.dek_b64 = parsed->getString("dek_b64").value_or(std::string{});
  bundle.relay_user_id = parsed->getString("relay_user_id").value_or(std::string{});
  bundle.nickname = parsed->getString("nickname").value_or(std::string{});
  bundle.created_at_ms = parsed->getIf<int64_t>("created_at").value_or(0);
  bundle.expires_at_ms = parsed->getIf<int64_t>("expires_at").value_or(0);

  bundle.account_card_json = parsed->getString("account_card_json");
  bundle.recovery_seed_b64 = parsed->getString("recovery_seed_b64");

  if (auto valid = Validate(bundle); !valid) {
    return valid.error();
  }
  return bundle;
}

} // namespace pbr
