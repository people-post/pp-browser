#include "common/DirectoryJson.h"

#include "common/PbrCompat.h"

namespace pbr {

std::string ContactIdKindToString(const ContactIdKind kind) {
  switch (kind) {
  case ContactIdKind::Account:
    return "account";
  case ContactIdKind::RelayUser:
    return "relay_user";
  case ContactIdKind::PeerId:
    return "peer_id";
  case ContactIdKind::Blockchain:
    return "blockchain";
  case ContactIdKind::Custom:
    return "custom";
  }
  return "account";
}

ContactIdKind ContactIdKindFromString(const std::string& value) {
  if (value == "account") {
    return ContactIdKind::Account;
  }
  if (value == "peer_id") {
    return ContactIdKind::PeerId;
  }
  if (value == "blockchain") {
    return ContactIdKind::Blockchain;
  }
  if (value == "custom") {
    return ContactIdKind::Custom;
  }
  if (value == "relay_user") {
    return ContactIdKind::RelayUser;
  }
  // Unknown kinds must not silently become relay_user (M009).
  return ContactIdKind::Custom;
}

void AppendContactIdsFromJson(const Object& json, std::vector<ContactId>& ids) {
  const Array* arr = json.getArray("ids");
  if (!arr) {
    return;
  }
  for (const Value& item_value : arr->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    ContactId id;
    if (auto kind = item->getString("kind")) {
      id.kind = ContactIdKindFromString(*kind);
    }
    if (auto value = item->getString("value")) {
      id.value = *value;
    }
    if (auto primary = item->getIf<bool>("primary")) {
      id.primary = *primary;
    }
    if (!id.value.empty()) {
      ids.push_back(std::move(id));
    }
  }
}

Value ContactIdsToJson(const std::vector<ContactId>& ids) {
  std::vector<Value> out;
  out.reserve(ids.size());
  for (const ContactId& id : ids) {
    Object row;
    row.set("kind", ContactIdKindToString(id.kind));
    row.set("value", id.value);
    row.set("primary", id.primary);
    out.push_back(ObjectValue(std::move(row)));
  }
  return ArrayValue(std::move(out));
}

Value MultiaddrsToJson(const std::vector<std::string>& multiaddrs) {
  std::vector<Value> out;
  out.reserve(multiaddrs.size());
  for (const std::string& ma : multiaddrs) {
    out.push_back(Value(ma));
  }
  return ArrayValue(std::move(out));
}

void ParseMultiaddrsArray(const Array& arr, std::vector<std::string>& multiaddrs) {
  for (const Value& item : arr.elements) {
    if (auto s = asString(item)) {
      multiaddrs.push_back(*s);
    }
  }
}

Value DirectoryEndpointsToJson(const std::vector<DirectoryEndpoint>& endpoints) {
  std::vector<Value> out;
  out.reserve(endpoints.size());
  for (const DirectoryEndpoint& endpoint : endpoints) {
    Object row;
    row.set("peer_id", endpoint.peer_id);
    row.set("multiaddrs", MultiaddrsToJson(endpoint.multiaddrs));
    row.set("updated_at", endpoint.updated_at);
    out.push_back(ObjectValue(std::move(row)));
  }
  return ArrayValue(std::move(out));
}

void ParseDirectoryEndpointsArray(const Array& arr, std::vector<DirectoryEndpoint>& endpoints) {
  for (const Value& item_value : arr.elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    DirectoryEndpoint endpoint;
    if (auto peer_id = item->getString("peer_id")) {
      endpoint.peer_id = *peer_id;
    }
    if (const Array* multiaddrs = item->getArray("multiaddrs")) {
      ParseMultiaddrsArray(*multiaddrs, endpoint.multiaddrs);
    }
    if (auto updated = item->getIf<int64_t>("updated_at")) {
      endpoint.updated_at = *updated;
    }
    if (!endpoint.peer_id.empty()) {
      endpoints.push_back(std::move(endpoint));
    }
  }
}

std::optional<ProfileIconRef> ProfileIconRefFromJson(const Object& json) {
  ProfileIconRef icon;
  if (auto url = json.getString("url")) {
    icon.url = *url;
  }
  if (auto blob_id = json.getString("blob_id")) {
    icon.blob_id = *blob_id;
  }
  if (auto kind = json.getString("kind")) {
    icon.kind = *kind;
  }
  if (icon.empty()) {
    return std::nullopt;
  }
  return icon;
}

Object ProfileIconRefToJson(const ProfileIconRef& icon) {
  Object out;
  if (!icon.url.empty()) {
    out.set("url", icon.url);
  }
  if (!icon.blob_id.empty()) {
    out.set("blob_id", icon.blob_id);
  }
  if (!icon.kind.empty()) {
    out.set("kind", icon.kind);
  }
  return out;
}

namespace {

bool HasRelayUserId(const std::vector<ContactId>& ids, const std::string& relay_user_id) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::RelayUser && id.value == relay_user_id) {
      return true;
    }
  }
  return false;
}

bool HasAccountId(const std::vector<ContactId>& ids, const std::string& account_id) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::Account && id.value == account_id) {
      return true;
    }
  }
  return false;
}

}  // namespace

Object DirectoryHitToJson(const DirectoryHit& hit) {
  Object out;
  out.set("hit_id", hit.hit_id);
  out.set("display_name", hit.display_name);
  out.set("nickname", hit.nickname);
  out.set("ids", ContactIdsToJson(hit.ids));
  out.set("endpoints", DirectoryEndpointsToJson(hit.endpoints));
  out.set("initiation_floor", hit.initiation_floor);
  if (hit.account_id && !hit.account_id->empty()) {
    out.set("account_id", *hit.account_id);
  }
  if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
    out.set("signing_public_key_b64", *hit.signing_public_key_b64);
  }
  if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
    out.set("kem_public_key_b64", *hit.kem_public_key_b64);
  }
  if (hit.icon && !hit.icon->empty()) {
    out.set("icon", ProfileIconRefToJson(*hit.icon));
  }
  return out;
}

DirectoryHit DirectoryHitFromJson(const Object& json) {
  DirectoryHit hit;
  if (auto hit_id = json.getString("hit_id")) {
    hit.hit_id = *hit_id;
  }
  if (auto display_name = json.getString("display_name")) {
    hit.display_name = *display_name;
  }
  if (auto nickname = json.getString("nickname")) {
    hit.nickname = *nickname;
  }
  AppendContactIdsFromJson(json, hit.ids);
  if (auto account_id = json.getString("account_id")) {
    if (!account_id->empty()) {
      hit.account_id = *account_id;
      if (!HasAccountId(hit.ids, *account_id)) {
        for (ContactId& id : hit.ids) {
          id.primary = false;
        }
        hit.ids.insert(hit.ids.begin(), {ContactIdKind::Account, *account_id, true});
      }
    }
  }
  if (auto relay_user_id = json.getString("relay_user_id")) {
    if (!relay_user_id->empty()) {
      if (hit.hit_id.empty()) {
        hit.hit_id = *relay_user_id;
      }
      if (!HasRelayUserId(hit.ids, *relay_user_id)) {
        hit.ids.push_back({ContactIdKind::RelayUser, *relay_user_id, hit.ids.empty()});
      }
    }
  }
  if (hit.display_name.empty() && !hit.nickname.empty()) {
    hit.display_name = hit.nickname;
  }
  if (auto signing = json.getString("signing_public_key_b64")) {
    hit.signing_public_key_b64 = *signing;
  }
  if (auto kem = json.getString("kem_public_key_b64")) {
    hit.kem_public_key_b64 = *kem;
  }
  if (const Array* endpoints = json.getArray("endpoints")) {
    ParseDirectoryEndpointsArray(*endpoints, hit.endpoints);
  }
  FlattenDirectoryEndpoints(hit.ids, hit.multiaddrs, hit.endpoints);
  if (auto floor = json.getIf<int64_t>("initiation_floor")) {
    hit.initiation_floor = *floor;
  }
  if (const Object* icon = json.getObject("icon")) {
    hit.icon = ProfileIconRefFromJson(*icon);
  }
  if (auto kind = json.getString("entity_kind")) {
    hit.entity_kind = *kind;
  }
  if (auto seq = json.getIf<int64_t>("seq")) {
    hit.seq = *seq;
  }
  if (auto expires = json.getString("expires_at")) {
    hit.expires_at = *expires;
  }
  if (const Object* caps = json.getObject("capabilities")) {
    hit.has_capabilities = true;
    hit.circuit_relay = caps->getIf<bool>("circuit_relay").value_or(false);
    hit.media_relay = caps->getIf<bool>("media_relay").value_or(false);
    hit.dht = caps->getIf<bool>("dht").value_or(false);
    hit.ledger_gateway = caps->getIf<bool>("ledger_gateway").value_or(false);
  }
  return hit;
}

}  // namespace pbr
