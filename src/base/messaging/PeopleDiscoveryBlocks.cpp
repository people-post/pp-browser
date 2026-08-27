#include "base/messaging/PeopleDiscoveryBlocks.h"

#include "base/people/ContactJson.h"
#include "common/ValueJson.h"

namespace pbr {

namespace {

std::string PrimaryRelayId(const std::vector<ContactId>& ids) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::RelayUser && id.primary) {
      return id.value;
    }
  }
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      return id.value;
    }
  }
  if (!ids.empty()) {
    return ids.front().value;
  }
  return {};
}

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

Value IdsToValue(const std::vector<ContactId>& ids) {
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

void AppendContactIdsFromObject(const Object& json, std::vector<ContactId>& ids) {
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

Value MultiaddrsToValue(const std::vector<std::string>& multiaddrs) {
  std::vector<Value> out;
  out.reserve(multiaddrs.size());
  for (const std::string& ma : multiaddrs) {
    out.push_back(Value(ma));
  }
  return ArrayValue(std::move(out));
}

void ParseMultiaddrsArray(const Array* arr, std::vector<std::string>& multiaddrs) {
  if (!arr) {
    return;
  }
  for (const Value& item : arr->elements) {
    if (auto ma = asString(item)) {
      multiaddrs.push_back(*ma);
    }
  }
}

Value EndpointsToValue(const std::vector<DirectoryEndpoint>& endpoints) {
  std::vector<Value> out;
  out.reserve(endpoints.size());
  for (const DirectoryEndpoint& endpoint : endpoints) {
    Object row;
    row.set("peer_id", endpoint.peer_id);
    row.set("multiaddrs", MultiaddrsToValue(endpoint.multiaddrs));
    row.set("updated_at", endpoint.updated_at);
    out.push_back(ObjectValue(std::move(row)));
  }
  return ArrayValue(std::move(out));
}

void ParseEndpointsArray(const Array* arr, std::vector<DirectoryEndpoint>& endpoints) {
  if (!arr) {
    return;
  }
  for (const Value& item_value : arr->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    DirectoryEndpoint endpoint;
    if (auto peer_id = item->getString("peer_id")) {
      endpoint.peer_id = *peer_id;
    }
    ParseMultiaddrsArray(item->getArray("multiaddrs"), endpoint.multiaddrs);
    if (auto updated_at = item->getIf<int64_t>("updated_at")) {
      endpoint.updated_at = *updated_at;
    }
    if (!endpoint.peer_id.empty()) {
      endpoints.push_back(std::move(endpoint));
    }
  }
}

std::optional<ProfileIconRef> ProfileIconRefFromObject(const Object& json) {
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

Object ProfileIconRefToObject(const ProfileIconRef& icon) {
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

Object DirectoryHitToObject(const DirectoryHit& hit) {
  Object out;
  out.set("hit_id", hit.hit_id);
  out.set("display_name", hit.display_name);
  out.set("nickname", hit.nickname);
  out.set("ids", IdsToValue(hit.ids));
  out.set("endpoints", EndpointsToValue(hit.endpoints));
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
    out.set("icon", ObjectValue(ProfileIconRefToObject(*hit.icon)));
  }
  return out;
}

DirectoryHit DirectoryHitFromObject(const Object& json) {
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
  AppendContactIdsFromObject(json, hit.ids);
  if (auto account_id = json.getString("account_id"); account_id && !account_id->empty()) {
    hit.account_id = *account_id;
    if (!HasAccountId(hit.ids, *account_id)) {
      for (ContactId& id : hit.ids) {
        id.primary = false;
      }
      hit.ids.insert(hit.ids.begin(), {ContactIdKind::Account, *account_id, true});
    }
  }
  if (auto relay_user_id = json.getString("relay_user_id"); relay_user_id && !relay_user_id->empty()) {
    if (hit.hit_id.empty()) {
      hit.hit_id = *relay_user_id;
    }
    if (!HasRelayUserId(hit.ids, *relay_user_id)) {
      hit.ids.push_back({ContactIdKind::RelayUser, *relay_user_id, hit.ids.empty()});
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
  ParseEndpointsArray(json.getArray("endpoints"), hit.endpoints);
  FlattenDirectoryEndpoints(hit.ids, hit.multiaddrs, hit.endpoints);
  if (auto floor = json.getIf<int64_t>("initiation_floor")) {
    hit.initiation_floor = *floor;
  }
  if (const Object* icon = json.getObject("icon")) {
    hit.icon = ProfileIconRefFromObject(*icon);
  }
  return hit;
}

Contact ContactFromObject(const Object& json) {
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
      AppendContactIdsFromObject(*remote, contact.remote.ids);
      ParseEndpointsArray(remote->getArray("endpoints"), contact.remote.endpoints);
      ParseMultiaddrsArray(remote->getArray("multiaddrs"), contact.remote.multiaddrs);
      if (auto fetched_at = remote->getIf<int64_t>("fetched_at")) {
        contact.remote.fetched_at = *fetched_at;
      }
      if (const Object* icon = remote->getObject("icon")) {
        contact.remote.icon = ProfileIconRefFromObject(*icon);
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
  AppendContactIdsFromObject(json, contact.remote.ids);
  if (auto relay_user_id = json.getString("relay_user_id"); relay_user_id && !relay_user_id->empty()) {
    if (!HasRelayUserId(contact.remote.ids, *relay_user_id)) {
      contact.remote.ids.push_back({ContactIdKind::RelayUser, *relay_user_id, true});
    }
  } else if (auto relay_id = json.getString("relay_id"); relay_id && !relay_id->empty()) {
    if (!HasRelayUserId(contact.remote.ids, *relay_id)) {
      contact.remote.ids.push_back({ContactIdKind::RelayUser, *relay_id, true});
    }
  }
  if (auto peer_id = json.getString("peer_id"); peer_id && !peer_id->empty()) {
    if (!HasPeerId(contact.remote.ids, *peer_id)) {
      contact.remote.ids.push_back({ContactIdKind::PeerId, *peer_id, contact.remote.ids.empty()});
    }
  }
  if (const Array* multiaddrs = json.getArray("multiaddrs")) {
    ParseMultiaddrsArray(multiaddrs, contact.remote.multiaddrs);
  } else if (auto multiaddr = json.getString("multiaddr")) {
    contact.remote.multiaddrs.push_back(*multiaddr);
  }
  contact.remote.fetched_at = 0;
  SyncContactMirrors(contact);
  return contact;
}

Value DirectoryHitItemActions(const DirectoryHit& hit) {
  const Object hit_json = DirectoryHitToObject(hit);
  std::vector<Value> actions;

  Object message_payload;
  message_payload.set("type", "start_conversation");
  message_payload.set("directory_hit", ObjectValue(Object(hit_json)));
  Object message_action;
  message_action.set("label", "Message");
  message_action.set("message", "Start chat with " + hit.display_name);
  message_action.set("payload", ObjectValue(std::move(message_payload)));
  actions.push_back(ObjectValue(std::move(message_action)));

  Object add_payload;
  add_payload.set("type", "add_contact");
  add_payload.set("directory_hit", ObjectValue(Object(hit_json)));
  Object add_action;
  add_action.set("label", "Add contact");
  add_action.set("message", "Add " + hit.display_name);
  add_action.set("payload", ObjectValue(std::move(add_payload)));
  actions.push_back(ObjectValue(std::move(add_action)));

  return ArrayValue(std::move(actions));
}

Value ContactItemActions(const Contact& contact) {
  std::vector<Value> actions;

  Object message_payload;
  message_payload.set("type", "start_conversation");
  message_payload.set("contact_id", contact.id);
  Object message_action;
  message_action.set("label", "Message");
  message_action.set("message", "Start chat with " + contact.display_name);
  message_action.set("payload", ObjectValue(std::move(message_payload)));
  actions.push_back(ObjectValue(std::move(message_action)));

  Object view_payload;
  view_payload.set("type", "show_contact");
  view_payload.set("contact_id", contact.id);
  Object view_action;
  view_action.set("label", "View IDs");
  view_action.set("message", "Show IDs for " + contact.display_name);
  view_action.set("payload", ObjectValue(std::move(view_payload)));
  actions.push_back(ObjectValue(std::move(view_action)));

  return ArrayValue(std::move(actions));
}

Object BuildLongListBlock(const std::string& title, Value items) {
  Object block;
  block.set("type", "long_list");
  block.set("title", title);
  block.set("items", std::move(items));
  return block;
}

std::string BuildBlocksJson(std::vector<Value> blocks) {
  Object root;
  root.set("blocks", ArrayValue(std::move(blocks)));
  return DumpJson(root);
}

} // namespace

std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<Contact>& contacts) {
  std::vector<Value> blocks;

  if (!directory_hits.empty()) {
    Object paragraph;
    paragraph.set("type", "paragraph");
    paragraph.set("text", "Here are people on the network:");
    blocks.push_back(ObjectValue(std::move(paragraph)));

    std::vector<Value> items;
    for (const DirectoryHit& hit : directory_hits) {
      Object item;
      item.set("title", hit.display_name);
      item.set("actions", DirectoryHitItemActions(hit));
      if (!hit.nickname.empty()) {
        item.set("subtitle", "@" + hit.nickname);
      }
      const std::string meta = PrimaryRelayId(hit.ids);
      if (!meta.empty()) {
        item.set("meta", meta);
      }
      items.push_back(ObjectValue(std::move(item)));
    }
    blocks.push_back(ObjectValue(BuildLongListBlock("Search results", ArrayValue(std::move(items)))));
  } else if (!contacts.empty()) {
    Object paragraph;
    paragraph.set("type", "paragraph");
    paragraph.set("text", "Your local contacts:");
    blocks.push_back(ObjectValue(std::move(paragraph)));

    std::vector<Value> items;
    for (const Contact& contact : contacts) {
      Object item;
      item.set("title", contact.display_name);
      item.set("actions", ContactItemActions(contact));
      if (!contact.server_nickname.empty()) {
        item.set("subtitle", "@" + contact.server_nickname);
      }
      const std::string meta = PrimaryRelayId(contact.ids);
      if (!meta.empty()) {
        item.set("meta", meta);
      }
      items.push_back(ObjectValue(std::move(item)));
    }
    blocks.push_back(ObjectValue(BuildLongListBlock("Contacts", ArrayValue(std::move(items)))));
  } else {
    Object paragraph;
    paragraph.set("type", "paragraph");
    paragraph.set("text", "No people found. Try a different search.");
    blocks.push_back(ObjectValue(std::move(paragraph)));
  }

  return BuildBlocksJson(std::move(blocks));
}

std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json) {
  auto parsed = ParseValue(raw_json);
  if (!parsed) {
    return {};
  }
  const Array* doc = asArray(*parsed);
  if (!doc || doc->elements.empty()) {
    return {};
  }

  std::vector<DirectoryHit> hits;
  std::vector<Contact> contacts;
  for (const Value& item_value : doc->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    if (item->contains("hit_id")) {
      hits.push_back(DirectoryHitFromObject(*item));
    } else if (item->contains("id") && item->contains("display_name")) {
      contacts.push_back(ContactFromObject(*item));
    }
  }

  if (hits.empty() && contacts.empty()) {
    return {};
  }
  return BuildPeopleDiscoveryBlocksJson(hits, contacts);
}

} // namespace pbr
