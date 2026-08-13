#include "base/crypto/LinkDeviceCodec.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/MlDsa.h"

#include <nlohmann/json.hpp>

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
  nlohmann::json json;
  json["format"] = kLinkDeviceFormat;
  json["account_id"] = bundle.account_id;
  json["account_ml_dsa_pk_b64"] = bundle.account_ml_dsa_pk_b64;
  json["account_ml_dsa_sk_b64"] = bundle.account_ml_dsa_sk_b64;
  json["dek_b64"] = bundle.dek_b64;
  json["relay_user_id"] = bundle.relay_user_id;
  json["nickname"] = bundle.nickname;
  json["created_at"] = bundle.created_at_ms;
  json["expires_at"] = bundle.expires_at_ms;
  nlohmann::json psks = nlohmann::json::array();
  for (const LinkDevicePublicPsk& row : bundle.public_psks) {
    nlohmann::json item = {{"peer_identity_kind", row.key.peer_identity_kind},
                           {"peer_identity_value", row.key.peer_identity_value},
                           {"channel", CryptoChannelToString(row.key.channel)},
                           {"session_epoch", row.session_epoch},
                           {"master_psk_b64", row.master_psk_b64}};
    if (row.psk_verified_at) {
      item["psk_verified_at"] = *row.psk_verified_at;
    }
    nlohmann::json retired = nlohmann::json::array();
    for (const RetiredPskEntry& entry : row.retired_psks) {
      retired.push_back({{"epoch", entry.epoch},
                         {"master_psk_b64", entry.master_psk_b64},
                         {"retired_at", entry.retired_at}});
    }
    item["retired_psks"] = std::move(retired);
    psks.push_back(std::move(item));
  }
  json["public_psks"] = std::move(psks);
  const std::string serialized = json.dump();
  if (serialized.size() > kMaxLinkDeviceBundleBytes) {
    return Error("Serialized link-device bundle exceeds size limit");
  }
  return serialized;
}

Roe<LinkDeviceBundleV1> LinkDeviceCodec::Parse(const std::string& json) {
  if (json.size() > kMaxLinkDeviceBundleBytes) {
    return Error("Link-device bundle exceeds size limit");
  }
  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return Error("Invalid link-device bundle JSON");
  }
  if (parsed.value("format", std::string{}) != kLinkDeviceFormat) {
    return Error("Unsupported link-device bundle format");
  }

  LinkDeviceBundleV1 bundle;
  bundle.account_id = parsed.value("account_id", std::string{});
  bundle.account_ml_dsa_pk_b64 = parsed.value("account_ml_dsa_pk_b64", std::string{});
  bundle.account_ml_dsa_sk_b64 = parsed.value("account_ml_dsa_sk_b64", std::string{});
  bundle.dek_b64 = parsed.value("dek_b64", std::string{});
  bundle.relay_user_id = parsed.value("relay_user_id", std::string{});
  bundle.nickname = parsed.value("nickname", std::string{});
  bundle.created_at_ms = parsed.value("created_at", static_cast<int64_t>(0));
  bundle.expires_at_ms = parsed.value("expires_at", static_cast<int64_t>(0));

  if (parsed.contains("public_psks") && parsed["public_psks"].is_array()) {
    for (const auto& item : parsed["public_psks"]) {
      if (!item.is_object()) {
        return Error("Invalid public PSK row in link bundle");
      }
      LinkDevicePublicPsk row;
      row.key.peer_identity_kind = item.value("peer_identity_kind", std::string{});
      row.key.peer_identity_value = item.value("peer_identity_value", std::string{});
      row.key.channel = ChannelFromString(item.value("channel", std::string{}));
      row.session_epoch = item.value("session_epoch", 0u);
      row.master_psk_b64 = item.value("master_psk_b64", std::string{});
      if (item.contains("psk_verified_at") && item["psk_verified_at"].is_number_integer()) {
        row.psk_verified_at = item["psk_verified_at"].get<int64_t>();
      }
      if (item.contains("retired_psks") && item["retired_psks"].is_array()) {
        for (const auto& retired : item["retired_psks"]) {
          RetiredPskEntry entry;
          entry.epoch = retired.value("epoch", 0u);
          entry.master_psk_b64 = retired.value("master_psk_b64", std::string{});
          entry.retired_at = retired.value("retired_at", static_cast<int64_t>(0));
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
