#include "lib/people/ContactJson.h"

#include "common/PbrCompat.h"

#include <vector>
#include "common/MeshAlias.h"

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
  if (!mirrored.remote.locator_relay.empty() && !mirrored.remote.locator_relay_peer_id.empty()) {
    Object locator_relay;
    locator_relay.set("origin", mirrored.remote.locator_relay);
    locator_relay.set("peer_id", mirrored.remote.locator_relay_peer_id);
    remote.set("locator_relay", ObjectValue(std::move(locator_relay)));
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

void DropRetiredRelayIds(Contact& contact) {
  std::erase_if(contact.remote.ids, [](const mesh::ContactId& id) {
    return id.kind == mesh::ContactIdKind::Custom && id.value.rfind("relay:", 0) == 0;
  });
  SyncContactMirrors(contact);
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
      if (const Object* locator_relay = remote->getObject("locator_relay")) {
        contact.remote.locator_relay = locator_relay->getString("origin").value_or("");
        contact.remote.locator_relay_peer_id = locator_relay->getString("peer_id").value_or("");
        if (contact.remote.locator_relay.empty() || contact.remote.locator_relay_peer_id.empty()) {
          contact.remote.locator_relay.clear(); // an address without its PeerId is none (MR008)
          contact.remote.locator_relay_peer_id.clear();
        }
      }
    }
    SyncContactMirrors(contact);
    return contact;
  }

  // A row without `local` (the flat form from before the local/remote split) is not read: only its id.
  contact.remote.fetched_at = 0;
  SyncContactMirrors(contact);
  return contact;
}

} // namespace pbr
