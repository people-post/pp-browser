#include "lib/messaging/GroupChange.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"
#include "lib/people/CardKeys.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_change {
namespace {

// The signing domain: the version is the byte after it, the JSON's `change_version`. The changes hash is not a
// signature: its tag keeps its `-v1`.
constexpr const char kChangeDomain[] = "pp-browser:group-roster-change";
constexpr const char kChangeVersionKey[] = "change_version";
constexpr SignedVersions kChangeVersions{.floor = kChangeVersion, .current = kChangeVersion};
constexpr const char kChangesHashTag[] = "pp-browser:group-changes-v1";

/** u32 big-endian length, then the bytes (as the owner's statement encodes). */
void Put(ByteVector& out, const std::string& field) {
  const auto n = static_cast<uint32_t>(field.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(n >> shift));
  }
  out.insert(out.end(), field.begin(), field.end());
}

void Put(ByteVector& out, const uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void PutIds(ByteVector& out, std::vector<std::string> ids) {
  std::sort(ids.begin(), ids.end());
  Put(out, static_cast<uint64_t>(ids.size()));
  for (const std::string& id : ids) {
    Put(out, id);
  }
}

void PutBody(ByteVector& out, const GroupChange& c) {
  switch (c.kind) {
    case GroupChangeKind::Add:
      Put(out, c.member);
      Put(out, c.invite_nonce);
      Put(out, c.link_id);
      Put(out, c.readmits);
      return;
    case GroupChangeKind::Leave:
      PutIds(out, c.adds);
      PutIds(out, c.keeps);
      return;
    case GroupChangeKind::Remove:
      Put(out, c.member);
      PutIds(out, c.adds);
      PutIds(out, c.keeps);
      return;
    case GroupChangeKind::Rename:
      Put(out, c.title);
      Put(out, c.title_version);
      return;
    case GroupChangeKind::LinkAdd:
      Put(out, c.link_id);
      Put(out, c.secret_hash);
      Put(out, static_cast<uint64_t>(c.expires_at));
      Put(out, c.max_uses);
      return;
    case GroupChangeKind::LinkRevoke:
      Put(out, c.link_id);
      return;
  }
}

bool DistinctBounded(const std::vector<std::string>& ids) {
  if (ids.size() > kMaxNamedChanges) {
    return false;
  }
  std::set<std::string> seen;
  return std::all_of(ids.begin(), ids.end(),
                     [&](const std::string& id) { return !id.empty() && seen.insert(id).second; });
}

/** Which body fields `kind` carries. */
struct BodyFields {
  bool member = false;
  bool invite = false;
  bool link_id = false;
  bool readmits = false;
  bool named = false;
  bool rename = false;
  bool link = false;
};

BodyFields FieldsOf(const GroupChangeKind kind) {
  switch (kind) {
    case GroupChangeKind::Add:
      return {.member = true, .invite = true, .link_id = true, .readmits = true};
    case GroupChangeKind::Leave:
      return {.named = true};
    case GroupChangeKind::Remove:
      return {.member = true, .named = true};
    case GroupChangeKind::Rename:
      return {.rename = true};
    case GroupChangeKind::LinkAdd:
      return {.link_id = true, .link = true};
    case GroupChangeKind::LinkRevoke:
      return {.link_id = true};
  }
  return {};
}

/** No field set that `kind` does not carry. */
bool NoForeignFields(const GroupChange& c) {
  const BodyFields f = FieldsOf(c.kind);
  return (f.member || c.member.empty()) && (f.invite || c.invite_nonce.empty()) && (f.link_id || c.link_id.empty()) &&
         (f.readmits || c.readmits.empty()) && (f.named || (c.adds.empty() && c.keeps.empty())) &&
         (f.rename || (c.title.empty() && c.title_version == 0)) &&
         (f.link || (c.secret_hash.empty() && c.expires_at == 0 && c.max_uses == 0));
}

Roe<void> CheckBody(const GroupChange& c) {
  switch (c.kind) {
    case GroupChangeKind::Add:
      if (c.member.empty() || c.member == c.signer) {
        return Error("roster change: an add names someone other than its signer");
      }
      if (c.invite_nonce.empty() == c.link_id.empty()) {
        return Error("roster change: an add carries its invite or its link, not both");
      }
      return {};
    case GroupChangeKind::Remove:
      if (c.member.empty() || c.member == c.signer) {
        return Error("roster change: a remove names someone other than its signer (a leave is its own change)");
      }
      [[fallthrough]];
    case GroupChangeKind::Leave:
      if (!DistinctBounded(c.adds) || !DistinctBounded(c.keeps)) {
        return Error("roster change: repeated, empty or too many named changes");
      }
      return {};
    case GroupChangeKind::Rename:
      if (c.title.empty() || c.title.size() > kMaxTitleBytes || c.title_version == 0) {
        return Error("roster change: a rename carries a title and a version");
      }
      return {};
    case GroupChangeKind::LinkAdd:
      if (c.link_id.empty() || c.secret_hash.empty() || c.expires_at <= 0) {
        return Error("roster change: a link carries its id, secret hash and expiry");
      }
      return {};
    case GroupChangeKind::LinkRevoke:
      if (c.link_id.empty()) {
        return Error("roster change: a revoke names its link");
      }
      return {};
  }
  return Error("roster change: unknown kind");
}

std::vector<Value> IdValues(const std::vector<std::string>& ids) {
  std::vector<Value> out;
  out.reserve(ids.size());
  for (const std::string& id : ids) {
    out.emplace_back(id);
  }
  return out;
}

Object BodyToJson(const GroupChange& c) {
  Object body;
  const auto set_if = [&](const char* key, const std::string& value) {
    if (!value.empty()) {
      body.set(key, value);
    }
  };
  set_if("member", c.member);
  set_if("invite_nonce", c.invite_nonce);
  set_if("link_id", c.link_id);
  set_if("readmits", c.readmits);
  set_if("title", c.title);
  set_if("secret_hash", c.secret_hash);
  const BodyFields f = FieldsOf(c.kind);
  if (f.named) {
    body.set("adds", ArrayValue(IdValues(c.adds)));
    body.set("keeps", ArrayValue(IdValues(c.keeps)));
  }
  if (f.rename) {
    body.setJsonUInt("title_version", c.title_version);
  }
  if (f.link) {
    body.setJsonUInt("expires_at", static_cast<uint64_t>(std::max<int64_t>(c.expires_at, 0)));
    if (c.max_uses > 0) {
      body.setJsonUInt("max_uses", c.max_uses);
    }
  }
  return body;
}

Roe<std::vector<std::string>> IdsFromJson(const Object& body, const char* key) {
  std::vector<std::string> ids;
  if (const Array* items = body.getArray(key)) {
    for (const Value& item : items->elements) {
      auto id = asString(item);
      if (!id) {
        return Error(std::string("roster change: ") + key + " holds a non-string");
      }
      ids.push_back(*id);
    }
  }
  return ids;
}

Roe<void> BodyFromJson(const Object& body, GroupChange& c) {
  c.member = body.getString("member").value_or("");
  c.invite_nonce = body.getString("invite_nonce").value_or("");
  c.link_id = body.getString("link_id").value_or("");
  c.readmits = body.getString("readmits").value_or("");
  c.title = body.getString("title").value_or("");
  c.title_version = body.getNonNegInt("title_version").value_or(0);
  c.secret_hash = body.getString("secret_hash").value_or("");
  c.expires_at = static_cast<int64_t>(body.getNonNegInt("expires_at").value_or(0));
  c.max_uses = body.getNonNegInt("max_uses").value_or(0);
  auto adds = IdsFromJson(body, "adds");
  auto keeps = IdsFromJson(body, "keeps");
  if (!adds) {
    return adds.error();
  }
  if (!keeps) {
    return keeps.error();
  }
  c.adds = std::move(*adds);
  c.keeps = std::move(*keeps);
  return {};
}

} // namespace

std::string KindToString(const GroupChangeKind kind) {
  switch (kind) {
    case GroupChangeKind::Add:
      return "add";
    case GroupChangeKind::Leave:
      return "leave";
    case GroupChangeKind::Remove:
      return "remove";
    case GroupChangeKind::Rename:
      return "rename";
    case GroupChangeKind::LinkAdd:
      return "link_add";
    case GroupChangeKind::LinkRevoke:
      return "link_revoke";
  }
  return "";
}

std::optional<GroupChangeKind> KindFromString(const std::string& value) {
  for (const GroupChangeKind kind : {GroupChangeKind::Add, GroupChangeKind::Leave, GroupChangeKind::Remove,
                                     GroupChangeKind::Rename, GroupChangeKind::LinkAdd, GroupChangeKind::LinkRevoke}) {
    if (KindToString(kind) == value) {
      return kind;
    }
  }
  return std::nullopt;
}

ByteVector SignBytes(const GroupChange& change) {
  ByteVector fields;
  Put(fields, change.group_id);
  Put(fields, change.base_epoch);
  Put(fields, change.signer);
  Put(fields, change.signer_device);
  Put(fields, static_cast<uint64_t>(change.at));
  Put(fields, KindToString(change.kind)); // a kind read is one of these: its string as received
  PutBody(fields, change);
  return SignedBytesWriter(kChangeDomain, kChangeVersion).Raw(fields).Finish(change.ext);
}

std::string Id(const GroupChange& change) {
  return BytesToHex(Blake2b256(SignBytes(change)));
}

Roe<void> CheckShape(const GroupChange& change) {
  if (change.group_id.empty() || change.signer.empty() || change.signer_device.empty()) {
    return Error("roster change: missing group, signer or device");
  }
  if (!NoForeignFields(change)) {
    return Error("roster change: carries a field its kind does not");
  }
  return CheckBody(change);
}

Roe<GroupChange> Sign(GroupChange change, const GroupDeviceSign& sign) {
  if (auto shape = CheckShape(change); !shape) {
    return shape.error();
  }
  auto signature = sign(SignBytes(change));
  if (!signature) {
    return signature.error();
  }
  auto typed = TypedSignatureB64(*signature);
  if (!typed) {
    return typed.error();
  }
  change.signature_b64 = std::move(typed.value());
  return change;
}

Roe<void> Verify(const GroupChange& change, const AccountCard& card, const int64_t now_ms) {
  if (card.keys.account_id != change.signer) {
    return Error("roster change: the card is not the signer's");
  }
  if (auto proved = VerifyAccountCard(card, now_ms); !proved) {
    return proved.error();
  }
  const AccountCardDevice* device = FindCardDevice(card, change.signer_device);
  if (!device) {
    return Error("roster change: the signing device is not on the signer's card");
  }
  return VerifyDeviceSignature(change, *device);
}

Roe<void> VerifyDeviceSignature(const GroupChange& change, const AccountCardDevice& device) {
  TypedRead key;
  TypedRead signature;
  return ReceiveSigned({
                           .what = "roster change",
                           .outside_version = kChangeVersion, // FromJson read `change_version`
                           .versions = kChangeVersions,
                           .parse = [&]() -> Roe<void> {
                             if (device.peer_id != change.signer_device) {
                               return Error("not the signing device");
                             }
                             return {};
                           },
                           .algorithms = [&]() -> Roe<void> {
                             key = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, device.signing_public_key_b64,
                                                    "roster change device key");
                             if (!key.ok()) {
                               return key.ToError();
                             }
                             signature = ReadTypedBlobB64(pp::BlobKind::kSignature, change.signature_b64,
                                                          "roster change signature");
                             if (!signature.ok()) {
                               return signature.ToError();
                             }
                             return {};
                           },
                           .verify = [&]() -> Roe<void> {
                             if (auto ok = VerifyTyped(key, SignBytes(change), signature); !ok) {
                               return Error("the signature does not verify for the signing device");
                             }
                             return {};
                           },
                           .ext = &change.ext,
                       })
      .ToRoe();
}

Object ToJson(const GroupChange& change) {
  Object json;
  json.set(kChangeVersionKey, int64_t{kChangeVersion});
  json.set("group_id", change.group_id);
  json.setJsonUInt("base_epoch", change.base_epoch);
  json.set("signer", change.signer);
  json.set("signer_device", change.signer_device);
  if (change.signer_card) {
    json.set("signer_card", ObjectValue(AccountCardToJson(*change.signer_card)));
  }
  json.setJsonUInt("at", static_cast<uint64_t>(std::max<int64_t>(change.at, 0)));
  json.set("change", KindToString(change.kind));
  json.set("body", ObjectValue(BodyToJson(change)));
  WriteExtB64(json, change.ext);
  json.set("signature", change.signature_b64);
  return json;
}

Roe<GroupChange> FromJson(const Object& json) {
  // 1. The version, before anything else is read.
  const std::optional<int64_t> version = OutsideVersionOfJson(json, kChangeVersionKey);
  if (!version || *version < kChangeVersion) {
    return Error("roster change made by an older app (no change_version 2)");
  }
  if (*version > kChangeVersion) {
    return AppError::NewerFormat("roster change version " + std::to_string(*version) + ": made by a newer app");
  }
  // 2. The fields and the extension area.
  GroupChange change;
  change.group_id = json.getString("group_id").value_or("");
  change.base_epoch = json.getNonNegInt("base_epoch").value_or(0);
  change.signer = json.getString("signer").value_or("");
  change.signer_device = json.getString("signer_device").value_or("");
  change.at = static_cast<int64_t>(json.getNonNegInt("at").value_or(0));
  change.signature_b64 = json.getString("signature").value_or("");
  const std::string kind_text = json.getString("change").value_or("");
  auto kind = KindFromString(kind_text);
  const Object* body = json.getObject("body");
  if (kind_text.empty() || !body || change.signature_b64.empty()) {
    return Error("roster change: missing kind, body or signature");
  }
  if (!kind) {
    // Its body is laid out by its kind, so its signed bytes cannot be rebuilt here: a newer app's change.
    return AppError::NewerFormat("roster change of kind '" + kind_text + "': made by a newer app");
  }
  change.kind = *kind;
  if (auto read = BodyFromJson(*body, change); !read) {
    return read.error();
  }
  if (const Object* card = json.getObject("signer_card")) {
    auto parsed = AccountCardFromJson(*card);
    if (!parsed) {
      return parsed.error();
    }
    change.signer_card = std::move(*parsed);
  }
  auto ext = ReadExtB64(json);
  if (!ext) {
    return Error("roster change: " + ext.error().message);
  }
  change.ext = std::move(ext.value());
  if (auto shape = CheckShape(change); !shape) {
    return shape.error();
  }
  // 3. The signature's algorithm id.
  if (const TypedRead signature =
          ReadTypedBlobB64(pp::BlobKind::kSignature, change.signature_b64, "roster change signature");
      !signature.ok()) {
    return signature.ToError();
  }
  return change;
}

std::string ChangesHash(std::vector<std::string> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  ByteVector bytes;
  Put(bytes, std::string(kChangesHashTag));
  PutIds(bytes, std::move(ids));
  return BytesToHex(Blake2b256(bytes));
}

} // namespace group_change
} // namespace pbr
