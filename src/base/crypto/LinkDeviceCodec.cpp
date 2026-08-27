#include "base/crypto/LinkDeviceCodec.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"
#include "base/crypto/MlDsa.h"
#include "common/ValueJson.h"

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

CryptoChannel ChannelFromString(const std::string& value) {
  return value == "e2e_public" ? CryptoChannel::E2ePublic : CryptoChannel::E2e;
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
  for (const LinkDevicePublicPsk& row : bundle.public_psks) {
    if (row.key.channel != CryptoChannel::E2ePublic) {
      return Error("Link bundle must not include private e2e PSKs");
    }
    if (row.key.peer_identity_kind.empty() || row.key.peer_identity_value.empty()) {
      return Error("Link bundle public PSK missing peer identity");
    }
    if (row.session_epoch == 0) {
      return Error("Link bundle public PSK missing session_epoch");
    }
    if (auto psk = RequireB64Size(row.master_psk_b64, kMasterPskSize, "Public PSK"); !psk) {
      return psk.error();
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
  std::vector<Value> psks;
  psks.reserve(bundle.public_psks.size());
  for (const LinkDevicePublicPsk& row : bundle.public_psks) {
    Object item;
    item.set("peer_identity_kind", row.key.peer_identity_kind);
    item.set("peer_identity_value", row.key.peer_identity_value);
    item.set("channel", CryptoChannelToString(row.key.channel));
    item.setJsonUInt("session_epoch", row.session_epoch);
    item.set("master_psk_b64", row.master_psk_b64);
    if (row.psk_verified_at) {
      item.set("psk_verified_at", *row.psk_verified_at);
    }
    std::vector<Value> retired;
    retired.reserve(row.retired_psks.size());
    for (const RetiredPskEntry& entry : row.retired_psks) {
      Object retired_item;
      retired_item.setJsonUInt("epoch", entry.epoch);
      retired_item.set("master_psk_b64", entry.master_psk_b64);
      retired_item.set("retired_at", entry.retired_at);
      retired.push_back(ObjectValue(std::move(retired_item)));
    }
    item.set("retired_psks", ArrayValue(std::move(retired)));
    psks.push_back(ObjectValue(std::move(item)));
  }
  json.set("public_psks", ArrayValue(std::move(psks)));
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

  if (const Array* public_psks = parsed->getArray("public_psks")) {
    for (const Value& item_value : public_psks->elements) {
      const Object* item = asObject(item_value);
      if (!item) {
        return Error("Invalid public PSK row in link bundle");
      }
      LinkDevicePublicPsk row;
      row.key.peer_identity_kind = item->getString("peer_identity_kind").value_or(std::string{});
      row.key.peer_identity_value = item->getString("peer_identity_value").value_or(std::string{});
      row.key.channel = ChannelFromString(item->getString("channel").value_or(std::string{}));
      row.session_epoch = static_cast<uint32_t>(item->getNonNegInt("session_epoch").value_or(0));
      row.master_psk_b64 = item->getString("master_psk_b64").value_or(std::string{});
      if (auto verified = item->getIf<int64_t>("psk_verified_at")) {
        row.psk_verified_at = *verified;
      }
      if (const Array* retired = item->getArray("retired_psks")) {
        for (const Value& retired_value : retired->elements) {
          const Object* retired_item = asObject(retired_value);
          if (!retired_item) {
            continue;
          }
          RetiredPskEntry entry;
          entry.epoch = static_cast<uint32_t>(retired_item->getNonNegInt("epoch").value_or(0));
          entry.master_psk_b64 = retired_item->getString("master_psk_b64").value_or(std::string{});
          entry.retired_at = retired_item->getIf<int64_t>("retired_at").value_or(0);
          row.retired_psks.push_back(std::move(entry));
        }
      }
      bundle.public_psks.push_back(std::move(row));
    }
  }

  if (auto valid = Validate(bundle); !valid) {
    return valid.error();
  }
  return bundle;
}

} // namespace pbr
