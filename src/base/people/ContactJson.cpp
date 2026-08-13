#include "base/people/ContactJson.h"

#include <nlohmann/json.hpp>

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

std::string TrustLevelToString(const TrustLevel level) {
  switch (level) {
  case TrustLevel::Unknown:
    return "unknown";
  case TrustLevel::Friendly:
    return "friendly";
  case TrustLevel::Blocked:
    return "blocked";
  }
  return "unknown";
}

TrustLevel TrustLevelFromString(const std::string& value) {
  if (value == "friendly") {
    return TrustLevel::Friendly;
  }
  if (value == "blocked") {
    return TrustLevel::Blocked;
  }
  return TrustLevel::Unknown;
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

bool HasPeerId(const std::vector<ContactId>& ids, const std::string& peer_id) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::PeerId && id.value == peer_id) {
      return true;
    }
  }
  return false;
}

void AppendContactIdsFromJson(const nlohmann::json& json, std::vector<ContactId>& ids) {
  if (!json.contains("ids") || !json["ids"].is_array()) {
    return;
  }
  for (const auto& item : json["ids"]) {
    if (!item.is_object()) {
      continue;
    }
    ContactId id;
    if (item.contains("kind") && item["kind"].is_string()) {
      id.kind = ContactIdKindFromString(item["kind"].get<std::string>());
    }
    if (item.contains("value") && item["value"].is_string()) {
      id.value = item["value"].get<std::string>();
    }
    if (item.contains("primary") && item["primary"].is_boolean()) {
      id.primary = item["primary"].get<bool>();
    }
    if (!id.value.empty()) {
      ids.push_back(std::move(id));
    }
  }
}

nlohmann::json IdsToJson(const std::vector<ContactId>& ids) {
  nlohmann::json out = nlohmann::json::array();
  for (const ContactId& id : ids) {
    out.push_back({{"kind", ContactIdKindToString(id.kind)}, {"value", id.value}, {"primary", id.primary}});
  }
  return out;
}

nlohmann::json MultiaddrsToJson(const std::vector<std::string>& multiaddrs) {
  nlohmann::json out = nlohmann::json::array();
  for (const std::string& ma : multiaddrs) {
    out.push_back(ma);
  }
  return out;
}

void ParseMultiaddrsArray(const nlohmann::json& json, std::vector<std::string>& multiaddrs) {
  if (!json.is_array()) {
    return;
  }
  for (const auto& item : json) {
    if (item.is_string()) {
      multiaddrs.push_back(item.get<std::string>());
    }
  }
}

} // namespace

nlohmann::json ContactToJson(const Contact& contact) {
  Contact mirrored = contact;
  SyncContactMirrors(mirrored);
  nlohmann::json remote = {{"nickname", mirrored.remote.nickname},
                           {"ids", IdsToJson(mirrored.remote.ids)},
                           {"multiaddrs", MultiaddrsToJson(mirrored.remote.multiaddrs)}};
  if (mirrored.remote.fetched_at > 0) {
    remote["fetched_at"] = mirrored.remote.fetched_at;
  }
  return {{"id", mirrored.id},
          {"local",
           {{"display_name", mirrored.local.display_name}, {"trust", TrustLevelToString(mirrored.local.trust)}}},
          {"remote", std::move(remote)},
          {"overrides", nlohmann::json::object()}};
}

Contact ContactFromJson(const nlohmann::json& json) {
  Contact contact;
  if (json.contains("id") && json["id"].is_string()) {
    contact.id = json["id"].get<std::string>();
  }

  if (json.contains("local") && json["local"].is_object()) {
    const auto& local = json["local"];
    if (local.contains("display_name") && local["display_name"].is_string()) {
      contact.local.display_name = local["display_name"].get<std::string>();
    }
    if (local.contains("trust") && local["trust"].is_string()) {
      contact.local.trust = TrustLevelFromString(local["trust"].get<std::string>());
    }
    if (json.contains("remote") && json["remote"].is_object()) {
      const auto& remote = json["remote"];
      if (remote.contains("nickname") && remote["nickname"].is_string()) {
        contact.remote.nickname = remote["nickname"].get<std::string>();
      }
      AppendContactIdsFromJson(remote, contact.remote.ids);
      if (remote.contains("multiaddrs")) {
        ParseMultiaddrsArray(remote["multiaddrs"], contact.remote.multiaddrs);
      }
      if (remote.contains("fetched_at") && remote["fetched_at"].is_number_integer()) {
        contact.remote.fetched_at = remote["fetched_at"].get<int64_t>();
      }
    }
    SyncContactMirrors(contact);
    return contact;
  }

  // Legacy flat contact (pre local/remote split).
  if (json.contains("display_name") && json["display_name"].is_string()) {
    contact.local.display_name = json["display_name"].get<std::string>();
  }
  if (json.contains("server_nickname") && json["server_nickname"].is_string()) {
    contact.remote.nickname = json["server_nickname"].get<std::string>();
  } else if (json.contains("nickname") && json["nickname"].is_string()) {
    contact.remote.nickname = json["nickname"].get<std::string>();
  }
  if (json.contains("trust") && json["trust"].is_string()) {
    contact.local.trust = TrustLevelFromString(json["trust"].get<std::string>());
  }
  AppendContactIdsFromJson(json, contact.remote.ids);
  if (json.contains("relay_user_id") && json["relay_user_id"].is_string()) {
    const std::string relay_user_id = json["relay_user_id"].get<std::string>();
    if (!relay_user_id.empty() && !HasRelayUserId(contact.remote.ids, relay_user_id)) {
      contact.remote.ids.push_back({ContactIdKind::RelayUser, relay_user_id, true});
    }
  } else if (json.contains("relay_id") && json["relay_id"].is_string()) {
    const std::string relay_id = json["relay_id"].get<std::string>();
    if (!relay_id.empty() && !HasRelayUserId(contact.remote.ids, relay_id)) {
      contact.remote.ids.push_back({ContactIdKind::RelayUser, relay_id, true});
    }
  }
  if (json.contains("peer_id") && json["peer_id"].is_string()) {
    const std::string peer_id = json["peer_id"].get<std::string>();
    if (!peer_id.empty() && !HasPeerId(contact.remote.ids, peer_id)) {
      contact.remote.ids.push_back({ContactIdKind::PeerId, peer_id, contact.remote.ids.empty()});
    }
  }
  if (json.contains("multiaddrs") && json["multiaddrs"].is_array()) {
    ParseMultiaddrsArray(json["multiaddrs"], contact.remote.multiaddrs);
  } else if (json.contains("multiaddr") && json["multiaddr"].is_string()) {
    contact.remote.multiaddrs.push_back(json["multiaddr"].get<std::string>());
  }
  contact.remote.fetched_at = 0;
  SyncContactMirrors(contact);
  return contact;
}

nlohmann::json DirectoryHitToJson(const DirectoryHit& hit) {
  nlohmann::json out = {{"hit_id", hit.hit_id},
                        {"display_name", hit.display_name},
                        {"nickname", hit.nickname},
                        {"ids", IdsToJson(hit.ids)},
                        {"multiaddrs", MultiaddrsToJson(hit.multiaddrs)},
                        {"initiation_floor", hit.initiation_floor}};
  if (hit.account_id && !hit.account_id->empty()) {
    out["account_id"] = *hit.account_id;
  }
  if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
    out["signing_public_key_b64"] = *hit.signing_public_key_b64;
  }
  if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
    out["kem_public_key_b64"] = *hit.kem_public_key_b64;
  }
  return out;
}

DirectoryHit DirectoryHitFromJson(const nlohmann::json& json) {
  DirectoryHit hit;
  if (json.contains("hit_id") && json["hit_id"].is_string()) {
    hit.hit_id = json["hit_id"].get<std::string>();
  }
  if (json.contains("display_name") && json["display_name"].is_string()) {
    hit.display_name = json["display_name"].get<std::string>();
  }
  if (json.contains("nickname") && json["nickname"].is_string()) {
    hit.nickname = json["nickname"].get<std::string>();
  }
  AppendContactIdsFromJson(json, hit.ids);
  if (json.contains("account_id") && json["account_id"].is_string()) {
    const std::string account_id = json["account_id"].get<std::string>();
    if (!account_id.empty()) {
      hit.account_id = account_id;
      if (!HasAccountId(hit.ids, account_id)) {
        // Insert Account as primary; demote prior primary flags.
        for (ContactId& id : hit.ids) {
          id.primary = false;
        }
        hit.ids.insert(hit.ids.begin(), {ContactIdKind::Account, account_id, true});
      }
    }
  }
  if (json.contains("relay_user_id") && json["relay_user_id"].is_string()) {
    const std::string relay_user_id = json["relay_user_id"].get<std::string>();
    if (!relay_user_id.empty()) {
      if (hit.hit_id.empty()) {
        hit.hit_id = relay_user_id;
      }
      if (!HasRelayUserId(hit.ids, relay_user_id)) {
        hit.ids.push_back({ContactIdKind::RelayUser, relay_user_id, false});
      }
    }
  }
  if (hit.display_name.empty() && !hit.nickname.empty()) {
    hit.display_name = hit.nickname;
  }
  if (json.contains("signing_public_key_b64") && json["signing_public_key_b64"].is_string()) {
    hit.signing_public_key_b64 = json["signing_public_key_b64"].get<std::string>();
  }
  if (json.contains("kem_public_key_b64") && json["kem_public_key_b64"].is_string()) {
    hit.kem_public_key_b64 = json["kem_public_key_b64"].get<std::string>();
  }
  if (json.contains("peer_id") && json["peer_id"].is_string()) {
    const std::string peer_id = json["peer_id"].get<std::string>();
    if (!peer_id.empty() && !HasPeerId(hit.ids, peer_id)) {
      hit.ids.push_back({ContactIdKind::PeerId, peer_id, false});
    }
  }
  if (json.contains("multiaddrs") && json["multiaddrs"].is_array()) {
    ParseMultiaddrsArray(json["multiaddrs"], hit.multiaddrs);
  }
  if (json.contains("initiation_floor") && json["initiation_floor"].is_number_integer()) {
    hit.initiation_floor = json["initiation_floor"].get<int64_t>();
  }
  return hit;
}

} // namespace pbr
