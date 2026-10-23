#include "lib/messaging/AccountCard.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"

#include <algorithm>
#include <set>
#include <utility>

namespace pbr {

namespace {

constexpr char kAccountCardDomain[] = "pp-browser:account-card-v1";
constexpr uint8_t kAccountCardSignVersion = 1;
constexpr int64_t kAccountCardVersion = 1;

void AppendU64(ByteVector& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void AppendLenUtf8(ByteVector& out, const std::string& value) {
  AppendU64(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}

Roe<std::string> RequiredString(const Object& json, const char* key) {
  auto value = json.getString(key);
  if (!value || value->empty()) {
    return Error(std::string("account card: missing ") + key);
  }
  return *value;
}

} // namespace

Object KeyCardToJson(const KeyCard& card) {
  Object json;
  json.set("account_id", card.account_id);
  json.set("signing_public_key_b64", card.signing_public_key_b64);
  json.set("kem_public_key_b64", card.kem_public_key_b64);
  json.set("kem_binding_signature_b64", card.kem_binding_signature_b64);
  return json;
}

Roe<KeyCard> KeyCardFromJson(const Object& json) {
  KeyCard card;
  for (auto [key, field] : {std::pair{"account_id", &card.account_id},
                            std::pair{"signing_public_key_b64", &card.signing_public_key_b64},
                            std::pair{"kem_public_key_b64", &card.kem_public_key_b64},
                            std::pair{"kem_binding_signature_b64", &card.kem_binding_signature_b64}}) {
    auto value = RequiredString(json, key);
    if (!value) {
      return value.error();
    }
    *field = std::move(value.value());
  }
  return card;
}

Object AccountCardToJson(const AccountCard& card) {
  Object json = KeyCardToJson(card.keys);
  json.set("card_version", kAccountCardVersion);
  if (!card.nickname.empty()) {
    json.set("nickname", card.nickname);
  }
  std::vector<Value> devices;
  devices.reserve(card.devices.size());
  for (const std::string& peer_id : card.devices) {
    Object row;
    row.set("peer_id", peer_id);
    devices.push_back(ObjectValue(std::move(row)));
  }
  json.set("devices", ArrayValue(std::move(devices)));
  json.set("issued_at", card.issued_at);
  json.set("card_signature_b64", card.card_signature_b64);
  return json;
}

Roe<AccountCard> AccountCardFromJson(const Object& json) {
  if (json.getIf<int64_t>("card_version").value_or(0) != kAccountCardVersion) {
    return Error("account card: unsupported card_version");
  }
  AccountCard card;
  auto keys = KeyCardFromJson(json);
  if (!keys) {
    return keys.error();
  }
  card.keys = std::move(keys.value());
  card.nickname = json.getString("nickname").value_or("");
  if (const Array* devices = json.getArray("devices")) {
    for (const Value& item : devices->elements) {
      const Object* row = asObject(item);
      auto peer_id = row ? row->getString("peer_id") : std::nullopt;
      if (!peer_id || peer_id->empty()) {
        return Error("account card: bad device");
      }
      card.devices.push_back(*peer_id);
    }
  }
  card.issued_at = json.getIf<int64_t>("issued_at").value_or(0);
  auto signature = RequiredString(json, "card_signature_b64");
  if (!signature) {
    return signature.error();
  }
  card.card_signature_b64 = std::move(signature.value());
  return card;
}

Roe<void> VerifyKeyCard(const KeyCard& card) {
  return VerifyKemBinding(card.account_id, card.signing_public_key_b64, card.kem_public_key_b64,
                          card.kem_binding_signature_b64);
}

ByteVector AccountCardSignBytes(const AccountCard& card) {
  auto signing = Base64Decode(card.keys.signing_public_key_b64);
  auto kem = Base64Decode(card.keys.kem_public_key_b64);
  if (!signing || signing.value().size() != kMlDsa65PublicKeyBytes || !kem ||
      kem.value().size() != kHybridKemPublicKeyBytes) {
    return {};
  }
  ByteVector bytes(kAccountCardDomain, kAccountCardDomain + sizeof(kAccountCardDomain)); // with its NUL
  bytes.push_back(kAccountCardSignVersion);
  bytes.insert(bytes.end(), signing.value().begin(), signing.value().end());
  bytes.insert(bytes.end(), kem.value().begin(), kem.value().end());
  AppendLenUtf8(bytes, card.nickname);
  std::vector<std::string> devices = card.devices;
  std::sort(devices.begin(), devices.end());
  AppendU64(bytes, devices.size());
  for (const std::string& peer_id : devices) {
    AppendLenUtf8(bytes, peer_id);
  }
  AppendU64(bytes, static_cast<uint64_t>(card.issued_at));
  return bytes;
}

Roe<void> VerifyAccountCard(const AccountCard& card, int64_t now_ms) {
  if (auto keys = VerifyKeyCard(card.keys); !keys) {
    return keys.error();
  }
  if (card.devices.size() > kMaxAccountCardDevices) {
    return Error("account card: too many devices");
  }
  if (std::set<std::string>(card.devices.begin(), card.devices.end()).size() != card.devices.size()) {
    return Error("account card: duplicate device");
  }
  if (card.issued_at <= 0 || card.issued_at > now_ms + kAccountCardClockSkewMs) {
    return Error("account card: issued in the future");
  }
  const ByteVector message = AccountCardSignBytes(card);
  auto signing = Base64Decode(card.keys.signing_public_key_b64);
  auto signature = Base64Decode(card.card_signature_b64);
  if (message.empty() || !signing || !signature || signature.value().size() != kMlDsa65SignatureBytes) {
    return Error("account card: malformed");
  }
  auto verified = MlDsa::Verify(signing.value(), message, signature.value());
  if (!verified || !verified.value()) {
    return Error("account card: the account did not sign it");
  }
  return {};
}

Roe<AccountCard> SignAccountCard(AccountCard card, const std::function<Roe<std::string>(const ByteVector&)>& sign) {
  const ByteVector message = AccountCardSignBytes(card);
  if (message.empty()) {
    return Error("account card: keys do not decode");
  }
  auto signature = sign(message);
  if (!signature) {
    return signature.error();
  }
  card.card_signature_b64 = std::move(signature.value());
  return card;
}

} // namespace pbr
