#include "common/chat/PeopleDiscoveryBlocks.h"

#include "common/directory/DirectoryJson.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>

namespace pbr {

namespace {

constexpr size_t kDefaultMaxVisible = 10;

std::string PrimaryIdentityValue(const std::vector<ContactId>& ids) {
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::Account && id.primary) {
      return id.value;
    }
  }
  for (const ContactId& id : ids) {
    if (id.kind == ContactIdKind::Account) {
      return id.value;
    }
  }
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

/** Mirrors PeerDisplayLabel::ShortRelayId — keep in sync. */
std::string ShortIdentityId(const std::string& relay_or_peer_id) {
  if (relay_or_peer_id.empty()) {
    return {};
  }
  constexpr size_t kKeep = 12;
  if (relay_or_peer_id.size() <= kKeep + 1) {
    return relay_or_peer_id;
  }
  constexpr const char* kPrefix = "relay:";
  constexpr size_t kPrefixLen = 6;
  if (relay_or_peer_id.compare(0, kPrefixLen, kPrefix) == 0) {
    const std::string rest = relay_or_peer_id.substr(kPrefixLen);
    if (rest.size() <= 8) {
      return relay_or_peer_id;
    }
    return std::string(kPrefix) + rest.substr(0, 8) + "…";
  }
  constexpr const char* kAccountPrefix = "account:";
  constexpr size_t kAccountPrefixLen = 8;
  if (relay_or_peer_id.compare(0, kAccountPrefixLen, kAccountPrefix) == 0) {
    const std::string rest = relay_or_peer_id.substr(kAccountPrefixLen);
    if (rest.size() <= 10) {
      return relay_or_peer_id;
    }
    return std::string(kAccountPrefix) + rest.substr(0, 10) + "…";
  }
  return relay_or_peer_id.substr(0, kKeep) + "…";
}

std::string PreferPersonId(const DirectoryHit& hit) {
  if (hit.account_id && !hit.account_id->empty()) {
    return *hit.account_id;
  }
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return PrimaryIdentityValue(hit.ids);
}

std::string PreferPersonId(const PeopleDiscoveryContactView& contact) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return PrimaryIdentityValue(contact.ids);
}

bool IdentityKnown(const std::vector<ContactId>& ids, const std::optional<std::string>& account_id,
                   const std::unordered_set<std::string>& known) {
  if (known.empty()) {
    return false;
  }
  if (account_id && known.count(*account_id) != 0) {
    return true;
  }
  for (const ContactId& id : ids) {
    if (!id.value.empty() && known.count(id.value) != 0) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> MatchingContactId(const std::vector<ContactId>& hit_ids,
                                             const std::optional<std::string>& account_id,
                                             const std::vector<PeopleDiscoveryContactView>& contacts) {
  auto matches = [&](const PeopleDiscoveryContactView& contact) {
    if (account_id) {
      for (const ContactId& id : contact.ids) {
        if (id.value == *account_id) {
          return true;
        }
      }
    }
    for (const ContactId& hit_id : hit_ids) {
      for (const ContactId& id : contact.ids) {
        if (!hit_id.value.empty() && hit_id.value == id.value) {
          return true;
        }
      }
    }
    return false;
  };
  for (const PeopleDiscoveryContactView& contact : contacts) {
    if (matches(contact) && !contact.id.empty()) {
      return contact.id;
    }
  }
  return std::nullopt;
}

std::string AvatarLetter(const std::string& display_name, const std::string& fallback_id) {
  for (unsigned char c : display_name) {
    if (std::isalpha(c)) {
      return std::string(1, static_cast<char>(std::toupper(c)));
    }
  }
  for (unsigned char c : fallback_id) {
    if (std::isalnum(c)) {
      return std::string(1, static_cast<char>(std::toupper(c)));
    }
  }
  return "?";
}

int AvatarTone(const std::string& stable_id) {
  if (stable_id.empty()) {
    return 0;
  }
  return static_cast<int>(std::hash<std::string>{}(stable_id) % 8);
}

void SetAvatarFields(Object& item, const std::string& display_name, const std::string& stable_id) {
  item.set("avatar_letter", AvatarLetter(display_name, stable_id));
  item.set("avatar_tone", static_cast<int64_t>(AvatarTone(stable_id.empty() ? display_name : stable_id)));
}

Object MakeAction(const std::string& label, const std::string& message, Object payload,
                  const std::string& style) {
  Object action;
  action.set("label", label);
  action.set("message", message);
  action.set("payload", ObjectValue(std::move(payload)));
  if (!style.empty()) {
    action.set("style", style);
  }
  return action;
}

Value DirectoryHitItemActions(const DirectoryHit& hit, const bool already_contact,
                              const std::optional<std::string>& contact_id) {
  const Object hit_json = DirectoryHitToJson(hit);
  std::vector<Value> actions;
  const std::string label_name =
      !hit.display_name.empty() ? hit.display_name : (!hit.nickname.empty() ? hit.nickname : "contact");

  if (already_contact) {
    Object message_payload;
    message_payload.set("type", "start_conversation");
    if (contact_id && !contact_id->empty()) {
      message_payload.set("contact_id", *contact_id);
    } else {
      message_payload.set("directory_hit", ObjectValue(Object(hit_json)));
    }
    actions.push_back(ObjectValue(
        MakeAction("Message", "Start chat with " + label_name, std::move(message_payload), "primary")));

    if (contact_id && !contact_id->empty()) {
      Object view_payload;
      view_payload.set("type", "show_contact");
      view_payload.set("contact_id", *contact_id);
      actions.push_back(
          ObjectValue(MakeAction("View", "Show " + label_name, std::move(view_payload), "secondary")));
    }
  } else {
    Object add_payload;
    add_payload.set("type", "add_contact");
    add_payload.set("directory_hit", ObjectValue(Object(hit_json)));
    actions.push_back(
        ObjectValue(MakeAction("Add contact", "Add " + label_name, std::move(add_payload), "primary")));

    Object message_payload;
    message_payload.set("type", "start_conversation");
    message_payload.set("directory_hit", ObjectValue(Object(hit_json)));
    actions.push_back(ObjectValue(
        MakeAction("Message", "Start chat with " + label_name, std::move(message_payload), "secondary")));
  }

  return ArrayValue(std::move(actions));
}

Value ContactItemActions(const PeopleDiscoveryContactView& contact) {
  std::vector<Value> actions;

  Object message_payload;
  message_payload.set("type", "start_conversation");
  message_payload.set("contact_id", contact.id);
  actions.push_back(ObjectValue(MakeAction("Message", "Start chat with " + contact.display_name,
                                           std::move(message_payload), "primary")));

  Object view_payload;
  view_payload.set("type", "show_contact");
  view_payload.set("contact_id", contact.id);
  actions.push_back(ObjectValue(
      MakeAction("View", "Show IDs for " + contact.display_name, std::move(view_payload), "secondary")));

  return ArrayValue(std::move(actions));
}

void FillPersonRowFields(Object& item, const std::string& display_name, const std::string& nickname,
                         const std::string& person_id, const bool already_contact) {
  std::string title = display_name;
  if (title.empty() && !nickname.empty()) {
    title = "~" + nickname;
  }
  if (title.empty()) {
    title = ShortIdentityId(person_id);
  }
  if (title.empty()) {
    title = "Unknown person";
  }
  item.set("title", title);

  std::ostringstream subtitle;
  if (!nickname.empty()) {
    const std::string nick_token = "~" + nickname;
    if (title != nick_token && title != nickname) {
      subtitle << nick_token;
    }
  }
  const std::string short_id = ShortIdentityId(person_id);
  if (!short_id.empty()) {
    if (subtitle.tellp() > 0) {
      subtitle << " · ";
    }
    subtitle << "@" << short_id;
  }
  const std::string subtitle_str = subtitle.str();
  if (!subtitle_str.empty()) {
    item.set("subtitle", subtitle_str);
  }

  if (already_contact) {
    item.set("meta", "In contacts");
  }

  SetAvatarFields(item, !display_name.empty() ? display_name : nickname, person_id);
}

Object BuildLongListBlock(const std::string& title, Value items, Value footer_actions = {}) {
  Object block;
  block.set("type", "long_list");
  block.set("title", title);
  block.set("items", std::move(items));
  if (const Array* footer = asArray(footer_actions); footer && !footer->elements.empty()) {
    block.set("footer_actions", std::move(footer_actions));
  }
  return block;
}

std::string BuildBlocksJson(std::vector<Value> blocks) {
  Object root;
  root.set("blocks", ArrayValue(std::move(blocks)));
  return DumpJson(root);
}

PeopleDiscoveryContactView ContactViewFromJson(const Object& json) {
  PeopleDiscoveryContactView contact;
  if (auto id = json.getString("id")) {
    contact.id = *id;
  }
  if (auto display_name = json.getString("display_name")) {
    contact.display_name = *display_name;
  }
  if (auto server_nickname = json.getString("server_nickname")) {
    contact.server_nickname = *server_nickname;
  }
  AppendContactIdsFromJson(json, contact.ids);
  return contact;
}

std::unordered_set<std::string> CollectKnownIdentities(const std::vector<PeopleDiscoveryContactView>& contacts,
                                                       const PeopleDiscoveryBuildOptions& options) {
  std::unordered_set<std::string> known = options.known_local_identity_values;
  for (const PeopleDiscoveryContactView& contact : contacts) {
    for (const ContactId& id : contact.ids) {
      if (!id.value.empty()) {
        known.insert(id.value);
      }
    }
  }
  return known;
}

Value RefineSearchFooter() {
  Object refine;
  refine.set("label", "Refine search…");
  refine.set("message", "Search for someone more specifically by nickname or account id");
  return ArrayValue({ObjectValue(std::move(refine))});
}

} // namespace

std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts,
                                           const PeopleDiscoveryBuildOptions& options) {
  std::vector<Value> blocks;
  const size_t max_visible =
      options.max_visible_items == 0 ? kDefaultMaxVisible : options.max_visible_items;
  const auto known = CollectKnownIdentities(contacts, options);

  if (!directory_hits.empty()) {
    const size_t total = directory_hits.size();
    const size_t visible = std::min(total, max_visible);

    Object paragraph;
    paragraph.set("type", "paragraph");
    if (total == 1) {
      paragraph.set("text", "Found 1 person on the network:");
    } else if (visible < total) {
      std::ostringstream text;
      text << "Found " << total << " people — showing the first " << visible
           << ". Open the panel to pick who you mean.";
      paragraph.set("text", text.str());
    } else {
      std::ostringstream text;
      text << "Found " << total << " people on the network:";
      paragraph.set("text", text.str());
    }
    blocks.push_back(ObjectValue(std::move(paragraph)));

    std::vector<Value> items;
    items.reserve(visible);
    for (size_t i = 0; i < visible; ++i) {
      const DirectoryHit& hit = directory_hits[i];
      const bool already =
          IdentityKnown(hit.ids, hit.account_id, known) ||
          MatchingContactId(hit.ids, hit.account_id, contacts).has_value();
      const auto contact_id = MatchingContactId(hit.ids, hit.account_id, contacts);

      Object item;
      FillPersonRowFields(item, hit.display_name, hit.nickname, PreferPersonId(hit), already);
      item.set("actions", DirectoryHitItemActions(hit, already, contact_id));
      items.push_back(ObjectValue(std::move(item)));
    }

    Value footer;
    if (visible < total) {
      footer = RefineSearchFooter();
    }
    blocks.push_back(ObjectValue(BuildLongListBlock(
        visible < total ? "Search results (partial)" : "Search results", ArrayValue(std::move(items)),
        std::move(footer))));
  } else if (!contacts.empty()) {
    const size_t total = contacts.size();
    const size_t visible = std::min(total, max_visible);

    Object paragraph;
    paragraph.set("type", "paragraph");
    if (total == 1) {
      paragraph.set("text", "1 local contact:");
    } else if (visible < total) {
      std::ostringstream text;
      text << total << " local contacts — showing the first " << visible << ".";
      paragraph.set("text", text.str());
    } else {
      std::ostringstream text;
      text << "Your local contacts (" << total << "):";
      paragraph.set("text", text.str());
    }
    blocks.push_back(ObjectValue(std::move(paragraph)));

    std::vector<Value> items;
    items.reserve(visible);
    for (size_t i = 0; i < visible; ++i) {
      const PeopleDiscoveryContactView& contact = contacts[i];
      Object item;
      FillPersonRowFields(item, contact.display_name, contact.server_nickname, PreferPersonId(contact),
                          false);
      item.set("actions", ContactItemActions(contact));
      items.push_back(ObjectValue(std::move(item)));
    }
    Value footer;
    if (visible < total) {
      footer = RefineSearchFooter();
    }
    blocks.push_back(
        ObjectValue(BuildLongListBlock("Contacts", ArrayValue(std::move(items)), std::move(footer))));
  } else {
    Object paragraph;
    paragraph.set("type", "paragraph");
    paragraph.set("text", "No people found. Try a different name, nickname, or account id.");
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
  std::vector<PeopleDiscoveryContactView> contacts;
  for (const Value& item_value : doc->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    if (item->contains("hit_id")) {
      hits.push_back(DirectoryHitFromJson(*item));
    } else if (item->contains("id") && item->contains("display_name")) {
      contacts.push_back(ContactViewFromJson(*item));
    }
  }

  if (hits.empty() && contacts.empty()) {
    return {};
  }
  return BuildPeopleDiscoveryBlocksJson(hits, contacts);
}

} // namespace pbr
