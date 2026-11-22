#include "lib/people/AccountCard.h"

#include "lib/people/CardKeys.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"
#include "crypto/PeerId.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <utility>

namespace pbr {

namespace {

constexpr std::string_view kHttpsScheme = "https://";
constexpr char kWhat[] = "account card";

Roe<std::string> RequiredString(const Object& json, const char* key) {
  auto value = json.getString(key);
  if (!value || value->empty()) {
    return Error(std::string("missing ") + key);
  }
  return *value;
}

Roe<std::vector<AccountCardDevice>> DevicesFromJson(const Object& json) {
  std::vector<AccountCardDevice> out;
  if (const Array* devices = json.getArray("devices")) {
    for (const Value& item : devices->elements) {
      const Object* row = asObject(item);
      if (!row) {
        return Error("bad device");
      }
      AccountCardDevice device;
      device.peer_id = row->getString("peer_id").value_or("");
      device.signing_public_key_b64 = row->getString("signing_public_key_b64").value_or("");
      device.kem_public_key_b64 = row->getString("kem_public_key_b64").value_or("");
      if (device.peer_id.empty() || device.signing_public_key_b64.empty() || device.kem_public_key_b64.empty()) {
        return Error("bad device");
      }
      out.push_back(std::move(device));
    }
  }
  return out;
}

/**
 * `relays` is required (an empty list is the mesh only), and each entry names an origin and a relay id of the `relay:`
 * form: a card without them is refused, as the relay does.
 */
Roe<std::vector<AccountCardRelay>> RelaysFromJson(const Object& json) {
  const Array* relays = json.getArray("relays");
  if (!relays) {
    return Error("missing relays");
  }
  std::vector<AccountCardRelay> out;
  for (const Value& item : relays->elements) {
    const Object* row = asObject(item);
    auto origin = row ? row->getString("origin") : std::nullopt;
    auto relay_id = row ? row->getString("relay_id") : std::nullopt;
    if (!origin) {
      return Error("a relay needs an origin");
    }
    if (!relay_id) {
      return Error("a relay needs a relay_id");
    }
    auto parsed = RelayId::Parse(*relay_id);
    if (!parsed) {
      return Error("a relay's relay_id is not a relay id");
    }
    out.push_back(AccountCardRelay{.origin = *origin, .relay_id = *parsed});
  }
  return out;
}

/** Step 2: the v5 fields, the extension area included. */
Roe<AccountCard> FieldsFromJson(const Object& json) {
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
    return Error("missing initiation_floor");
  }
  card.initiation_floor = *floor;
  card.issued_at = json.getIf<int64_t>("issued_at").value_or(0);
  auto ext = ReadExtB64(json);
  if (!ext) {
    return ext.error();
  }
  card.ext = std::move(ext.value());
  auto signature = RequiredString(json, "card_signature_b64");
  if (!signature) {
    return signature.error();
  }
  card.card_signature_b64 = std::move(signature.value());
  return card;
}

/** The typed blobs step 4 verifies with. */
struct CardBlobs {
  TypedRead signing;
  TypedRead signature;
};

/** Step 3: every typed blob names an algorithm this build knows (else NewerFormat) and is of its size. */
Roe<void> ReadCardBlobs(const AccountCard& card, CardBlobs& blobs) {
  blobs.signing =
      ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, card.keys.signing_public_key_b64, "the account signing key");
  if (!blobs.signing.ok()) {
    return blobs.signing.ToError();
  }
  if (auto kem = RawKemPublicKey(card.keys.kem_public_key_b64, "the account KEM key"); !kem) {
    return kem.error();
  }
  blobs.signature = ReadTypedBlobB64(pp::BlobKind::kSignature, card.card_signature_b64, "the card signature");
  if (!blobs.signature.ok()) {
    return blobs.signature.ToError();
  }
  for (const AccountCardDevice& device : card.devices) {
    if (auto signing = RawSigningPublicKey(device.signing_public_key_b64, "a device's signing key"); !signing) {
      return signing.error();
    }
    if (auto kem = RawKemPublicKey(device.kem_public_key_b64, "a device's KEM key"); !kem) {
      return kem.error();
    }
  }
  return {};
}

/** Step 4: the account's signature over the rebuilt bytes, its area included. */
Roe<void> VerifyCardSignature(const AccountCard& card, const CardBlobs& blobs) {
  const ByteVector message = AccountCardSignBytes(card);
  if (message.empty()) {
    return Error("its keys do not decode");
  }
  if (auto verified = VerifyTyped(blobs.signing, message, blobs.signature); !verified) {
    return Error("the account did not sign it");
  }
  return {};
}

SignedReceipt Refused(const SignedVerdict verdict, const std::string& why) {
  return SignedReceipt{.verdict = verdict,
                       .version = kAccountCardVersion,
                       .detail = std::string(kWhat) + " v" + std::to_string(kAccountCardVersion) + ": " + why};
}

/** Step 6: the key card, the device list, the relays, the floor and the clock. */
SignedReceipt CheckSemantics(const AccountCard& card, const int64_t now_ms) {
  const SignedReceipt binding =
      ReceiveKemBinding(card.keys.signing_public_key_b64, card.keys.kem_public_key_b64, card.keys.kem_binding_b64);
  if (!binding.accepted()) {
    return Refused(binding.verdict, "key card: " + binding.detail);
  }
  auto signing = RawSigningPublicKey(card.keys.signing_public_key_b64);
  auto account = signing ? AccountIdFromMlDsaPublicKey(*signing) : Roe<std::string>(signing.error());
  if (!account || *account != card.keys.account_id) {
    return Refused(SignedVerdict::Forged, "key card: the signing key is not the account's");
  }
  if (card.devices.empty()) {
    return Refused(SignedVerdict::Malformed, "no device");
  }
  if (card.devices.size() > kMaxAccountCardDevices) {
    return Refused(SignedVerdict::Malformed, "too many devices");
  }
  const std::vector<std::string> peer_ids = CardDevicePeerIds(card);
  if (std::set<std::string>(peer_ids.begin(), peer_ids.end()).size() != peer_ids.size()) {
    return Refused(SignedVerdict::Malformed, "duplicate device");
  }
  for (const AccountCardDevice& device : card.devices) {
    if (auto checked = VerifyCardDevice(device); !checked) {
      return Refused(AppError::IsNewerFormat(checked.error()) ? SignedVerdict::NeedsUpdate : SignedVerdict::Malformed,
                     checked.error().message);
    }
  }
  if (auto relays = VerifyCardRelays(card.relays); !relays) {
    return Refused(SignedVerdict::Malformed, relays.error().message);
  }
  if (card.initiation_floor < 0 || card.initiation_floor > kMaxCardInitiationFloor) {
    return Refused(SignedVerdict::Malformed, "initiation_floor out of range");
  }
  if (card.issued_at <= 0 || card.issued_at > now_ms + kAccountCardClockSkewMs) {
    return Refused(SignedVerdict::Malformed, "issued in the future");
  }
  return SignedReceipt{.verdict = SignedVerdict::Accepted, .version = kAccountCardVersion};
}

/** Steps 1–3 for a card's JSON (AccountCardFromJson); `card` gets its fields. */
SignedReceipt ReadCard(const Object& json, AccountCard& card) {
  CardBlobs blobs;
  return ReceiveSigned(SignedReceiveSteps{
      .what = kWhat,
      .outside_version = OutsideVersionOfJson(json, "card_version"),
      .versions = {.floor = kAccountCardVersion, .current = kAccountCardVersion},
      .parse = [&]() -> Roe<void> {
        auto fields = FieldsFromJson(json);
        if (!fields) {
          return fields.error();
        }
        card = std::move(fields.value());
        return {};
      },
      .algorithms = [&]() { return ReadCardBlobs(card, blobs); },
      // Steps 4 and 5 are VerifyAccountCard's: reading a card checks nothing it says.
      .verify = []() -> Roe<void> { return {}; },
  });
}

/** Steps 3–6 for a card read or made here. */
SignedReceipt CheckCard(const AccountCard& card, const int64_t now_ms) {
  CardBlobs blobs;
  const SignedReceipt received = ReceiveSigned(SignedReceiveSteps{
      .what = kWhat,
      .outside_version = kAccountCardVersion, // a card in memory is of the version this build writes
      .versions = {.floor = kAccountCardVersion, .current = kAccountCardVersion},
      .algorithms = [&]() { return ReadCardBlobs(card, blobs); },
      .verify = [&]() { return VerifyCardSignature(card, blobs); },
      .ext = &card.ext,
  });
  if (!received.accepted()) {
    return received;
  }
  return CheckSemantics(card, now_ms);
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
  std::set<RelayId> relay_ids;
  for (const AccountCardRelay& relay : relays) {
    if (!IsCardRelayOrigin(relay.origin)) {
      return Error("account card: a relay origin is not an https:// URL within the bounds");
    }
    if (relay.relay_id.empty()) {
      return Error("account card: a relay names no relay id");
    }
    if (!seen.insert(CardRelayKey(relay.origin)).second || !relay_ids.insert(relay.relay_id).second) {
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
    row.set("relay_id", relay.relay_id.ToString());
    relays.push_back(ObjectValue(std::move(row)));
  }
  json.set("relays", ArrayValue(std::move(relays)));
  json.set("initiation_floor", card.initiation_floor);
  json.set("issued_at", card.issued_at);
  WriteExtB64(json, card.ext);
  json.set("card_signature_b64", card.card_signature_b64);
  return json;
}

bool IsNewerAccountCard(const Object& json) {
  AccountCard card;
  return ReadCard(json, card).needs_update();
}

Roe<AccountCard> AccountCardFromJson(const Object& json) {
  AccountCard card;
  if (const SignedReceipt read = ReadCard(json, card); !read.accepted()) {
    return read.ToError();
  }
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
  auto signing = RawSigningPublicKey(device.signing_public_key_b64, "account card: a device's signing key");
  if (!signing) {
    return signing.error();
  }
  if (auto kem = RawKemPublicKey(device.kem_public_key_b64, "account card: a device's KEM key"); !kem) {
    return kem.error();
  }
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(*signing); // the raw key: PeerIds do not change with typing
  if (!peer_id || *peer_id != device.peer_id) {
    return Error("account card: a device's PeerId is not its signing key's");
  }
  return {};
}

Roe<void> VerifyKeyCard(const KeyCard& card) {
  return VerifyKemBinding(card.account_id, card.signing_public_key_b64, card.kem_public_key_b64, card.kem_binding_b64);
}

ByteVector AccountCardSignBytes(const AccountCard& card) {
  const auto typed = [](const std::string& b64) -> std::optional<ByteVector> {
    if (b64.empty()) {
      return std::nullopt;
    }
    auto bytes = Base64Decode(b64);
    return bytes ? std::optional<ByteVector>(std::move(bytes.value())) : std::nullopt;
  };
  const auto signing = typed(card.keys.signing_public_key_b64);
  const auto kem = typed(card.keys.kem_public_key_b64);
  if (!signing || !kem) {
    return {};
  }
  SignedBytesWriter out(kAccountCardDomain, static_cast<uint8_t>(kAccountCardVersion));
  out.Typed(*signing).Typed(*kem).Utf8(card.nickname);
  std::vector<AccountCardDevice> devices = card.devices;
  std::sort(devices.begin(), devices.end(),
            [](const AccountCardDevice& a, const AccountCardDevice& b) { return a.peer_id < b.peer_id; });
  out.U64(devices.size());
  for (const AccountCardDevice& device : devices) {
    const auto device_signing = typed(device.signing_public_key_b64);
    const auto device_kem = typed(device.kem_public_key_b64);
    if (!device_signing || !device_kem) {
      return {};
    }
    out.Utf8(device.peer_id).Typed(*device_signing).Typed(*device_kem);
  }
  out.U64(card.relays.size()); // in list order: the first is the preferred relay
  for (const AccountCardRelay& relay : card.relays) {
    out.Utf8(relay.origin).Raw(relay.relay_id.raw());
  }
  out.I64(card.initiation_floor).I64(card.issued_at);
  return out.Finish(card.ext);
}

Roe<void> VerifyAccountCard(const AccountCard& card, const int64_t now_ms) {
  return CheckCard(card, now_ms).ToRoe();
}

SignedReceipt ReceiveAccountCard(const Object& json, const int64_t now_ms, AccountCard* card) {
  AccountCard read_card;
  if (const SignedReceipt read = ReadCard(json, read_card); !read.accepted()) {
    return read;
  }
  const SignedReceipt checked = CheckCard(read_card, now_ms);
  if (checked.accepted() && card != nullptr) {
    *card = std::move(read_card);
  }
  return checked;
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
  auto raw = Base64Decode(*signature);
  if (!raw) {
    return Error("account card: the signature is not base64");
  }
  auto typed = WriteTypedBlobB64(pp::BlobKind::kSignature, *raw);
  if (!typed) {
    return typed.error();
  }
  card.card_signature_b64 = std::move(typed.value());
  return card;
}

} // namespace pbr
