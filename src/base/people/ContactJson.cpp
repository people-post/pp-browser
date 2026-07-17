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
  }
  if (json.contains("trust") && json["trust"].is_string()) {
    contact.trust = TrustLevelFromString(json["trust"].get<std::string>());
  }
  if (json.contains("ids") && json["ids"].is_array()) {
    for (const auto& item : json["ids"]) {
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
      contact.ids.push_back(std::move(id));
    }
  }
  if (json.contains("multiaddrs") && json["multiaddrs"].is_array()) {
    for (const auto& item : json["multiaddrs"]) {
      if (item.is_string()) {
        contact.multiaddrs.push_back(item.get<std::string>());
      }
    }
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
  return {{"hit_id", hit.hit_id},
          {"display_name", hit.display_name},
          {"nickname", hit.nickname},
          {"ids", std::move(ids)},
          {"multiaddrs", std::move(multiaddrs)}};
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
  if (json.contains("ids") && json["ids"].is_array()) {
    for (const auto& item : json["ids"]) {
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
      hit.ids.push_back(std::move(id));
    }
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
  return hit;
}

} // namespace pbr
