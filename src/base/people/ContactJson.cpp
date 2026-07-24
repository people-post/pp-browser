#include "base/people/ContactJson.h"

#include <nlohmann/json.hpp>

namespace pbr {

std::string ContactIdKindToString(const ContactIdKind kind) {
  switch (kind) {
  case ContactIdKind::RelayUser:
    return "relay_user";
  case ContactIdKind::PeerId:
    return "peer_id";
  case ContactIdKind::Blockchain:
    return "blockchain";
  case ContactIdKind::Custom:
    return "custom";
  }
  return "relay_user";
}

ContactIdKind ContactIdKindFromString(const std::string& value) {
  if (value == "peer_id") {
    return ContactIdKind::PeerId;
  }
  if (value == "blockchain") {
    return ContactIdKind::Blockchain;
  }
  if (value == "custom") {
    return ContactIdKind::Custom;
  }
  return ContactIdKind::RelayUser;
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

nlohmann::json ContactToJson(const Contact& contact) {
  nlohmann::json ids = nlohmann::json::array();
  for (const ContactId& id : contact.ids) {
    ids.push_back({{"kind", ContactIdKindToString(id.kind)}, {"value", id.value}, {"primary", id.primary}});
  }
  nlohmann::json multiaddrs = nlohmann::json::array();
  for (const std::string& ma : contact.multiaddrs) {
    multiaddrs.push_back(ma);
  }
  return {{"id", contact.id},
          {"display_name", contact.display_name},
          {"server_nickname", contact.server_nickname},
          {"ids", std::move(ids)},
          {"multiaddrs", std::move(multiaddrs)},
          {"trust", TrustLevelToString(contact.trust)}};
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

} // namespace

Contact ContactFromJson(const nlohmann::json& json) {
  Contact contact;
  if (json.contains("id") && json["id"].is_string()) {
    contact.id = json["id"].get<std::string>();
  }
  if (json.contains("display_name") && json["display_name"].is_string()) {
    contact.display_name = json["display_name"].get<std::string>();
  }
  if (json.contains("server_nickname") && json["server_nickname"].is_string()) {
    contact.server_nickname = json["server_nickname"].get<std::string>();
  } else if (json.contains("nickname") && json["nickname"].is_string()) {
    // Legacy flat field used before server_nickname.
    contact.server_nickname = json["nickname"].get<std::string>();
  }
  if (json.contains("trust") && json["trust"].is_string()) {
    contact.trust = TrustLevelFromString(json["trust"].get<std::string>());
  }
  AppendContactIdsFromJson(json, contact.ids);
  // Legacy flat identity fields (pre-ids[] address book).
  if (json.contains("relay_user_id") && json["relay_user_id"].is_string()) {
    const std::string relay_user_id = json["relay_user_id"].get<std::string>();
    if (!relay_user_id.empty() && !HasRelayUserId(contact.ids, relay_user_id)) {
      contact.ids.push_back({ContactIdKind::RelayUser, relay_user_id, true});
    }
  } else if (json.contains("relay_id") && json["relay_id"].is_string()) {
    const std::string relay_id = json["relay_id"].get<std::string>();
    if (!relay_id.empty() && !HasRelayUserId(contact.ids, relay_id)) {
      contact.ids.push_back({ContactIdKind::RelayUser, relay_id, true});
    }
  }
  if (json.contains("peer_id") && json["peer_id"].is_string()) {
    const std::string peer_id = json["peer_id"].get<std::string>();
    if (!peer_id.empty() && !HasPeerId(contact.ids, peer_id)) {
      contact.ids.push_back({ContactIdKind::PeerId, peer_id, contact.ids.empty()});
    }
  }
  if (json.contains("multiaddrs") && json["multiaddrs"].is_array()) {
    for (const auto& item : json["multiaddrs"]) {
      if (item.is_string()) {
        contact.multiaddrs.push_back(item.get<std::string>());
      }
    }
  } else if (json.contains("multiaddr") && json["multiaddr"].is_string()) {
    contact.multiaddrs.push_back(json["multiaddr"].get<std::string>());
  }
  return contact;
}

nlohmann::json DirectoryHitToJson(const DirectoryHit& hit) {
  nlohmann::json ids = nlohmann::json::array();
  for (const ContactId& id : hit.ids) {
    ids.push_back({{"kind", ContactIdKindToString(id.kind)}, {"value", id.value}, {"primary", id.primary}});
  }
  nlohmann::json multiaddrs = nlohmann::json::array();
  for (const std::string& ma : hit.multiaddrs) {
    multiaddrs.push_back(ma);
  }
  nlohmann::json out = {{"hit_id", hit.hit_id},
                        {"display_name", hit.display_name},
                        {"nickname", hit.nickname},
                        {"ids", std::move(ids)},
                        {"multiaddrs", std::move(multiaddrs)}};
  if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
    out["signing_public_key_b64"] = *hit.signing_public_key_b64;
  }
  if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
    out["kem_public_key_b64"] = *hit.kem_public_key_b64;
  }
  if (hit.app_version && !hit.app_version->empty()) {
    out["app_version"] = *hit.app_version;
  }
  if (hit.protocol_gen) {
    out["protocol_gen"] = *hit.protocol_gen;
  }
  if (hit.min_peer_protocol_gen) {
    out["min_peer_protocol_gen"] = *hit.min_peer_protocol_gen;
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
  // Wire/API shape from GET /v1/search and GET /v1/users/:id (relay_user_id + nickname).
  if (json.contains("relay_user_id") && json["relay_user_id"].is_string()) {
    const std::string relay_user_id = json["relay_user_id"].get<std::string>();
    if (!relay_user_id.empty()) {
      if (hit.hit_id.empty()) {
        hit.hit_id = relay_user_id;
      }
      if (!HasRelayUserId(hit.ids, relay_user_id)) {
        hit.ids.push_back({ContactIdKind::RelayUser, relay_user_id, true});
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
  if (json.contains("multiaddrs") && json["multiaddrs"].is_array()) {
    for (const auto& item : json["multiaddrs"]) {
      if (item.is_string()) {
        hit.multiaddrs.push_back(item.get<std::string>());
      }
    }
  }
  if (json.contains("app_version") && json["app_version"].is_string()) {
    hit.app_version = json["app_version"].get<std::string>();
  }
  if (json.contains("protocol_gen") && json["protocol_gen"].is_number_integer()) {
    hit.protocol_gen = json["protocol_gen"].get<int>();
  }
  if (json.contains("min_peer_protocol_gen") && json["min_peer_protocol_gen"].is_number_integer()) {
    hit.min_peer_protocol_gen = json["min_peer_protocol_gen"].get<int>();
  }
  return hit;
}

} // namespace pbr
