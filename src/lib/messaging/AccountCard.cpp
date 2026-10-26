#include "lib/messaging/AccountCard.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"
#include "crypto/PeerId.h"

#include <algorithm>
#include <set>
#include <utility>

namespace pbr {

namespace {

constexpr char kAccountCardDomain[] = "pp-browser:account-card-v2";
constexpr uint8_t kAccountCardSignVersion = 2;
constexpr int64_t kAccountCardVersion = 2;

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

Object AccountCardToJson(const AccountCard& card) {
  Object json = KeyCardToJson(card.keys);
  json.set("card_version", kAccountCardVersion);
  if (!card.nickname.empty()) {
    json.set("nickname", card.nickname);
  }
  std::vector<Value> devices;
  devices.reserve(card.devices.size());
  for (const AccountCardDevice& device : card.devices) {
    Object row;
    row.set("peer_id", device.peer_id);
    row.set("signing_public_key_b64", device.signing_public_key_b64);
    row.set("kem_public_key_b64", device.kem_public_key_b64);
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
      if (!row) {
        return Error("account card: bad device");
      }
      AccountCardDevice device;
      device.peer_id = row->getString("peer_id").value_or("");
      device.signing_public_key_b64 = row->getString("signing_public_key_b64").value_or("");
      device.kem_public_key_b64 = row->getString("kem_public_key_b64").value_or("");
      if (device.peer_id.empty() || device.signing_public_key_b64.empty() || device.kem_public_key_b64.empty()) {
        return Error("account card: bad device");
      }
      card.devices.push_back(std::move(device));
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

const AccountCardDevice* FindCardDevice(const AccountCard& card, const std::string& peer_id) {
  for (const AccountCardDevice& device : card.devices) {
    if (device.peer_id == peer_id) {
      return &device;
    }
  }
  return nullptr;
}

std::vector<std::string> CardDevicePeerIds(const AccountCard& card) {
  std::vector<std::string> ids;
  ids.reserve(card.devices.size());
  for (const AccountCardDevice& device : card.devices) {
    ids.push_back(device.peer_id);
  }
  return ids;
}

Roe<void> VerifyCardDevice(const AccountCardDevice& device) {
  auto signing = Base64Decode(device.signing_public_key_b64);
  auto kem = Base64Decode(device.kem_public_key_b64);
  if (!signing || signing.value().size() != kMlDsa65PublicKeyBytes || !kem ||
      kem.value().size() != kHybridKemPublicKeyBytes) {
    return Error("account card: a device's keys do not decode");
  }
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(signing.value());
  if (!peer_id || *peer_id != device.peer_id) {
    return Error("account card: a device's PeerId is not its signing key's");
  }
  return {};
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
  std::vector<AccountCardDevice> devices = card.devices;
  std::sort(devices.begin(), devices.end(),
            [](const AccountCardDevice& a, const AccountCardDevice& b) { return a.peer_id < b.peer_id; });
  AppendU64(bytes, devices.size());
  for (const AccountCardDevice& device : devices) {
    auto device_signing = Base64Decode(device.signing_public_key_b64);
    auto device_kem = Base64Decode(device.kem_public_key_b64);
    if (!device_signing || device_signing.value().size() != kMlDsa65PublicKeyBytes || !device_kem ||
        device_kem.value().size() != kHybridKemPublicKeyBytes) {
      return {};
    }
    AppendLenUtf8(bytes, device.peer_id);
    bytes.insert(bytes.end(), device_signing.value().begin(), device_signing.value().end());
    bytes.insert(bytes.end(), device_kem.value().begin(), device_kem.value().end());
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
  if (card.devices.empty()) {
    return Error("account card: no device");
  }
  const std::vector<std::string> peer_ids = CardDevicePeerIds(card);
  if (std::set<std::string>(peer_ids.begin(), peer_ids.end()).size() != peer_ids.size()) {
    return Error("account card: duplicate device");
  }
  for (const AccountCardDevice& device : card.devices) {
    if (auto checked = VerifyCardDevice(device); !checked) {
      return checked.error();
    }
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
