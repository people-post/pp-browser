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

Value DirectoryHitItemActions(const DirectoryHit& hit) {
  const Object hit_json = DirectoryHitToJson(hit);
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
      hits.push_back(DirectoryHitFromJson(*item));
    } else if (item->contains("id") && item->contains("display_name")) {
      contacts.push_back(ContactFromJson(*item));
    }
  }

  if (hits.empty() && contacts.empty()) {
    return {};
  }
  return BuildPeopleDiscoveryBlocksJson(hits, contacts);
}

} // namespace pbr
