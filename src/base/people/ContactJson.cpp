#include "base/people/ContactJson.h"

#include "common/PbrCompat.h"

namespace pbr {

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

bool HasPeerId(const std::vector<ContactId>& ids, const std::string& peer_id) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::PeerId && id.value == peer_id) {
      return true;
    }
  }
  return false;
}

}  // namespace

Object ContactToJson(const Contact& contact) {
  Contact mirrored = contact;
  SyncContactMirrors(mirrored);
  Object remote;
  remote.set("nickname", mirrored.remote.nickname);
  remote.set("ids", ContactIdsToJson(mirrored.remote.ids));
  remote.set("endpoints", DirectoryEndpointsToJson(mirrored.remote.endpoints));
  remote.set("multiaddrs", MultiaddrsToJson(mirrored.remote.multiaddrs));
  if (mirrored.remote.fetched_at > 0) {
    remote.set("fetched_at", mirrored.remote.fetched_at);
  }
  if (mirrored.remote.icon && !mirrored.remote.icon->empty()) {
    remote.set("icon", ProfileIconRefToJson(*mirrored.remote.icon));
  }

  Object local;
  local.set("display_name", mirrored.local.display_name);
  local.set("trust", TrustLevelToString(mirrored.local.trust));

  Object out;
  out.set("id", mirrored.id);
  out.set("local", local);
  out.set("remote", remote);
  out.set("overrides", Object{});
  return out;
}

Contact ContactFromJson(const Object& json) {
  Contact contact;
  if (auto id = json.getString("id")) {
    contact.id = *id;
  }

  if (const Object* local = json.getObject("local")) {
    if (auto display_name = local->getString("display_name")) {
      contact.local.display_name = *display_name;
    }
    if (auto trust = local->getString("trust")) {
      contact.local.trust = TrustLevelFromString(*trust);
    }
    if (const Object* remote = json.getObject("remote")) {
      if (auto nickname = remote->getString("nickname")) {
        contact.remote.nickname = *nickname;
      }
      AppendContactIdsFromJson(*remote, contact.remote.ids);
      if (const Array* endpoints = remote->getArray("endpoints")) {
        ParseDirectoryEndpointsArray(*endpoints, contact.remote.endpoints);
      }
      if (const Array* multiaddrs = remote->getArray("multiaddrs")) {
        ParseMultiaddrsArray(*multiaddrs, contact.remote.multiaddrs);
      }
      if (auto fetched_at = remote->getIf<int64_t>("fetched_at")) {
        contact.remote.fetched_at = *fetched_at;
      }
      if (const Object* icon = remote->getObject("icon")) {
        contact.remote.icon = ProfileIconRefFromJson(*icon);
      }
    }
    SyncContactMirrors(contact);
    return contact;
  }

  // Legacy flat contact (pre local/remote split).
  if (auto display_name = json.getString("display_name")) {
    contact.local.display_name = *display_name;
  }
  if (auto server_nickname = json.getString("server_nickname")) {
    contact.remote.nickname = *server_nickname;
  } else if (auto nickname = json.getString("nickname")) {
    contact.remote.nickname = *nickname;
  }
  if (auto trust = json.getString("trust")) {
    contact.local.trust = TrustLevelFromString(*trust);
  }
  AppendContactIdsFromJson(json, contact.remote.ids);
  if (auto relay_user_id = json.getString("relay_user_id")) {
    if (!relay_user_id->empty() && !HasRelayUserId(contact.remote.ids, *relay_user_id)) {
      contact.remote.ids.push_back({ContactIdKind::RelayUser, *relay_user_id, true});
    }
  } else if (auto relay_id = json.getString("relay_id")) {
    if (!relay_id->empty() && !HasRelayUserId(contact.remote.ids, *relay_id)) {
      contact.remote.ids.push_back({ContactIdKind::RelayUser, *relay_id, true});
    }
  }
  if (auto peer_id = json.getString("peer_id")) {
    if (!peer_id->empty() && !HasPeerId(contact.remote.ids, *peer_id)) {
      contact.remote.ids.push_back({ContactIdKind::PeerId, *peer_id, contact.remote.ids.empty()});
    }
  }
  if (const Array* multiaddrs = json.getArray("multiaddrs")) {
    ParseMultiaddrsArray(*multiaddrs, contact.remote.multiaddrs);
  } else if (auto multiaddr = json.getString("multiaddr")) {
    contact.remote.multiaddrs.push_back(*multiaddr);
  }
  contact.remote.fetched_at = 0;
  SyncContactMirrors(contact);
  return contact;
}

}  // namespace pbr
