#include "base/messaging/PeopleDiscoveryBlocks.h"

#include "base/messaging/MessagingJson.h"

#include <nlohmann/json.hpp>

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

nlohmann::json DirectoryHitItemActions(const DirectoryHit& hit) {
  const nlohmann::json hit_json = DirectoryHitToJson(hit);
  return nlohmann::json::array({
      {{"label", "Message"},
       {"message", "Start chat with " + hit.display_name},
       {"payload", nlohmann::json{{"type", "start_conversation"}, {"directory_hit", hit_json}}}},
      {{"label", "Add contact"},
       {"message", "Add " + hit.display_name},
       {"payload", nlohmann::json{{"type", "add_contact"}, {"directory_hit", hit_json}}}},
  });
}

nlohmann::json ContactItemActions(const Contact& contact) {
  return nlohmann::json::array({
      {{"label", "Message"},
       {"message", "Start chat with " + contact.display_name},
       {"payload", nlohmann::json{{"type", "start_conversation"}, {"contact_id", contact.id}}}},
      {{"label", "View IDs"},
       {"message", "Show IDs for " + contact.display_name},
       {"payload", nlohmann::json{{"type", "show_contact"}, {"contact_id", contact.id}}}},
  });
}

nlohmann::json BuildLongListBlock(const std::string& title, nlohmann::json items) {
  return {{"type", "long_list"}, {"title", title}, {"items", std::move(items)}};
}

std::string BuildBlocksJson(nlohmann::json blocks) {
  return nlohmann::json{{"blocks", std::move(blocks)}}.dump();
}

} // namespace

std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<Contact>& contacts) {
  nlohmann::json blocks = nlohmann::json::array();

  if (!directory_hits.empty()) {
    blocks.push_back(nlohmann::json{{"type", "paragraph"}, {"text", "Here are people on the network:"}});
    nlohmann::json items = nlohmann::json::array();
    for (const DirectoryHit& hit : directory_hits) {
      nlohmann::json item = {{"title", hit.display_name}, {"actions", DirectoryHitItemActions(hit)}};
      if (!hit.nickname.empty()) {
        item["subtitle"] = "@" + hit.nickname;
      }
      const std::string meta = PrimaryRelayId(hit.ids);
      if (!meta.empty()) {
        item["meta"] = meta;
      }
      items.push_back(std::move(item));
    }
    blocks.push_back(BuildLongListBlock("Search results", std::move(items)));
  } else if (!contacts.empty()) {
    blocks.push_back(nlohmann::json{{"type", "paragraph"}, {"text", "Your local contacts:"}});
    nlohmann::json items = nlohmann::json::array();
    for (const Contact& contact : contacts) {
      nlohmann::json item = {{"title", contact.display_name}, {"actions", ContactItemActions(contact)}};
      if (!contact.server_nickname.empty()) {
        item["subtitle"] = "@" + contact.server_nickname;
      }
      const std::string meta = PrimaryRelayId(contact.ids);
      if (!meta.empty()) {
        item["meta"] = meta;
      }
      items.push_back(std::move(item));
    }
    blocks.push_back(BuildLongListBlock("Contacts", std::move(items)));
  } else {
    blocks.push_back(nlohmann::json{{"type", "paragraph"}, {"text", "No people found. Try a different search."}});
  }

  return BuildBlocksJson(std::move(blocks));
}

std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json) {
  const nlohmann::json doc = nlohmann::json::parse(raw_json, nullptr, false);
  if (doc.is_discarded() || !doc.is_array() || doc.empty()) {
    return {};
  }

  std::vector<DirectoryHit> hits;
  std::vector<Contact> contacts;
  for (const auto& item : doc) {
    if (!item.is_object()) {
      continue;
    }
    if (item.contains("hit_id")) {
      hits.push_back(DirectoryHitFromJson(item));
    } else if (item.contains("id") && item.contains("display_name")) {
      contacts.push_back(ContactFromJson(item));
    }
  }

  if (hits.empty() && contacts.empty()) {
    return {};
  }
  return BuildPeopleDiscoveryBlocksJson(hits, contacts);
}

} // namespace pbr
