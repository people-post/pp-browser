#include "lib/people/AccountCard.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"
#include "crypto/PeerId.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <utility>

namespace pbr {

namespace {

constexpr char kAccountCardDomain[] = "pp-browser:account-card-v3";
constexpr uint8_t kAccountCardSignVersion = 3;
constexpr int64_t kAccountCardVersion = 3;
constexpr std::string_view kHttpsScheme = "https://";

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

Roe<std::vector<AccountCardDevice>> DevicesFromJson(const Object& json) {
  std::vector<AccountCardDevice> out;
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
      out.push_back(std::move(device));
    }
  }
  return out;
}

/** `relays` is required (an empty list is the mesh only): a card without it is refused, as the relay does. */
Roe<std::vector<AccountCardRelay>> RelaysFromJson(const Object& json) {
  const Array* relays = json.getArray("relays");
  if (!relays) {
    return Error("account card: missing relays");
  }
  std::vector<AccountCardRelay> out;
  for (const Value& item : relays->elements) {
    const Object* row = asObject(item);
    auto origin = row ? row->getString("origin") : std::nullopt;
    if (!origin) {
      return Error("account card: a relay needs an origin");
    }
    out.push_back(AccountCardRelay{.origin = *origin});
  }
  return out;
}

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

/** The host and port of an origin IsCardRelayOrigin accepts (between the scheme and the path). */
std::string_view Authority(std::string_view origin) {
  const std::string_view rest = origin.substr(kHttpsScheme.size());
  return rest.substr(0, rest.find('/'));
}

/** An authority's port is empty or a number up to 65535, and its host is not empty (`[…]` for an IPv6 address). */
bool WellFormedAuthority(std::string_view authority) {
  if (authority.empty() || authority.find('@') != std::string_view::npos) {
    return false;
  }
  size_t host_end = authority.size();
  if (authority.front() == '[') {
    host_end = authority.find(']');
    if (host_end == std::string_view::npos || host_end < 2) {
      return false;
    }
    ++host_end;
  } else {
    host_end = std::min(authority.find(':'), authority.size());
  }
  if (host_end == 0) {
    return false;
  }
  const std::string_view port = authority.substr(host_end);
  if (port.empty()) {
    return true;
  }
  if (port.front() != ':' || port.size() < 2 || port.size() > 6 ||
      !std::all_of(port.begin() + 1, port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  return std::stoi(std::string(port.substr(1))) <= 65535;
}

/** Two origins name the same relay when their keys match: scheme and host without case, `:443` dropped, `/` path. */
std::string CardRelayKey(std::string_view origin) {
  std::string authority = LowerAscii(Authority(origin));
  if (authority.size() > 4 && authority.ends_with(":443")) {
    authority.resize(authority.size() - 4);
  }
  const std::string_view path = origin.substr(kHttpsScheme.size() + Authority(origin).size());
  return authority + (path.empty() ? std::string("/") : std::string(path));
}

} // namespace

bool IsCardRelayOrigin(const std::string& origin) {
  if (origin.size() > kMaxCardRelayOriginBytes || origin.size() <= kHttpsScheme.size() ||
      LowerAscii(std::string_view(origin).substr(0, kHttpsScheme.size())) != kHttpsScheme) {
    return false;
  }
  const bool clean = std::none_of(origin.begin(), origin.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte <= 0x20 || byte == 0x7f || c == '?' || c == '#' || c == '\\';
  });
  return clean && WellFormedAuthority(Authority(origin));
}

Roe<void> VerifyCardRelays(const std::vector<AccountCardRelay>& relays) {
  if (relays.size() > kMaxAccountCardRelays) {
    return Error("account card: more than " + std::to_string(kMaxAccountCardRelays) + " relays");
  }
  std::set<std::string> seen;
  for (const AccountCardRelay& relay : relays) {
    if (!IsCardRelayOrigin(relay.origin)) {
      return Error("account card: a relay origin is not an https:// URL within the bounds");
    }
    if (!seen.insert(CardRelayKey(relay.origin)).second) {
      return Error("account card: duplicate relay");
    }
  }
  return {};
}

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
  std::vector<Value> relays;
  relays.reserve(card.relays.size());
  for (const AccountCardRelay& relay : card.relays) {
    Object row;
    row.set("origin", relay.origin);
    relays.push_back(ObjectValue(std::move(row)));
  }
  json.set("relays", ArrayValue(std::move(relays)));
  json.set("initiation_floor", card.initiation_floor);
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
  auto devices = DevicesFromJson(json);
  if (!devices) {
    return devices.error();
  }
  card.devices = std::move(devices.value());
  auto relays = RelaysFromJson(json);
  if (!relays) {
    return relays.error();
  }
  card.relays = std::move(relays.value());
  auto floor = json.getIf<int64_t>("initiation_floor");
  if (!floor) {
    return Error("account card: missing initiation_floor");
  }
  card.initiation_floor = *floor;
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
  AppendU64(bytes, card.relays.size()); // in list order: the first is the preferred relay
  for (const AccountCardRelay& relay : card.relays) {
    AppendLenUtf8(bytes, relay.origin);
  }
  AppendU64(bytes, static_cast<uint64_t>(card.initiation_floor));
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
  if (auto relays = VerifyCardRelays(card.relays); !relays) {
    return relays.error();
  }
  if (card.initiation_floor < 0 || card.initiation_floor > kMaxCardInitiationFloor) {
    return Error("account card: initiation_floor out of range");
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
