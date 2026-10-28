#include "common/chat/PeopleDiscoveryBlocks.h"

#include "common/directory/DirectoryJson.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>

namespace pbr {

namespace {

constexpr size_t kDefaultMaxVisible = 10;
// The list is stored with the message (capped at kMaxComposeTextBytes); a row is ~0.5 KB, so this keeps it well under.
constexpr size_t kMaxListedHits = 100;

std::string Fill(std::string templ, const std::string& name, const std::string& value) {
  const std::string token = "{" + name + "}";
  for (size_t pos = templ.find(token); pos != std::string::npos; pos = templ.find(token, pos + value.size())) {
    templ.replace(pos, token.size(), value);
  }
  return templ;
}

std::string PrimaryIdentityValue(const std::vector<mesh::ContactId>& ids) {
  for (const mesh::ContactId& id : ids) {
    if (id.kind == mesh::ContactIdKind::Account && id.primary) {
      return id.value;
    }
  }
  for (const mesh::ContactId& id : ids) {
    if (id.kind == mesh::ContactIdKind::Account) {
      return id.value;
    }
  }
  for (const mesh::ContactId& id : ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser && id.primary) {
      return id.value;
    }
  }
  for (const mesh::ContactId& id : ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser) {
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

std::string PreferPersonId(const mesh::DirectoryHit& hit) {
  if (hit.account_id && !hit.account_id->empty()) {
    return *hit.account_id;
  }
  for (const mesh::ContactId& id : hit.ids) {
    if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return PrimaryIdentityValue(hit.ids);
}

std::string PreferPersonId(const PeopleDiscoveryContactView& contact) {
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return PrimaryIdentityValue(contact.ids);
}

bool IdentityKnown(const std::vector<mesh::ContactId>& ids, const std::optional<std::string>& account_id,
                   const std::unordered_set<std::string>& known) {
  if (known.empty()) {
    return false;
  }
  if (account_id && known.count(*account_id) != 0) {
    return true;
  }
  for (const mesh::ContactId& id : ids) {
    if (!id.value.empty() && known.count(id.value) != 0) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> MatchingContactId(const std::vector<mesh::ContactId>& hit_ids,
                                             const std::optional<std::string>& account_id,
                                             const std::vector<PeopleDiscoveryContactView>& contacts) {
  auto matches = [&](const PeopleDiscoveryContactView& contact) {
    if (account_id) {
      for (const mesh::ContactId& id : contact.ids) {
        if (id.value == *account_id) {
          return true;
        }
      }
    }
    for (const mesh::ContactId& hit_id : hit_ids) {
      for (const mesh::ContactId& id : contact.ids) {
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

/** The hit's account id, picked as ContactsStore::AddFromDirectoryHit picks it; empty when it has none. */
std::string HitAccountId(const mesh::DirectoryHit& hit) {
  if (hit.account_id && !hit.account_id->empty()) {
    return *hit.account_id;
  }
  std::string account_id;
  for (const mesh::ContactId& id : hit.ids) {
    if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
      account_id = id.value;
      if (id.primary) {
        break;
      }
    }
  }
  return account_id;
}

/**
 * Add / Message on a directory row name the person only: the list is stored with the message (capped at
 * kMaxComposeTextBytes), and a hit's keys are ~8.6 KB. ContactActionDispatcher looks the account up when used.
 */
Object PersonActionPayload(const std::string& type, const mesh::DirectoryHit& hit) {
  Object payload;
  payload.set("type", type);
  if (const std::string account_id = HitAccountId(hit); !account_id.empty()) {
    payload.set("account_id", account_id);
  }
  payload.set("hit_id", hit.hit_id);
  payload.set("nickname", hit.nickname);
  payload.set("display_name", hit.display_name);
  return payload;
}

Value DirectoryHitItemActions(const mesh::DirectoryHit& hit, const bool already_contact,
                              const std::optional<std::string>& contact_id,
                              const PeopleDiscoveryLabels& labels) {
  std::vector<Value> actions;
  const std::string label_name =
      !hit.display_name.empty() ? hit.display_name : (!hit.nickname.empty() ? hit.nickname : "contact");

  if (already_contact) {
    Object message_payload;
    if (contact_id && !contact_id->empty()) {
      message_payload.set("type", "start_conversation");
      message_payload.set("contact_id", *contact_id);
    } else {
      message_payload = PersonActionPayload("start_conversation", hit);
    }
    actions.push_back(ObjectValue(
        MakeAction(labels.message, Fill(labels.start_chat_with, "name", label_name),
                                           std::move(message_payload), "primary")));

    if (contact_id && !contact_id->empty()) {
      Object view_payload;
      view_payload.set("type", "show_contact");
      view_payload.set("contact_id", *contact_id);
      actions.push_back(
          ObjectValue(MakeAction(labels.view, Fill(labels.show_name, "name", label_name),
                                                  std::move(view_payload), "secondary")));
    }
  } else {
    actions.push_back(
        ObjectValue(MakeAction(labels.add_contact, Fill(labels.add_name, "name", label_name),
                                         PersonActionPayload("add_contact", hit), "primary")));

    actions.push_back(ObjectValue(
        MakeAction(labels.message, Fill(labels.start_chat_with, "name", label_name),
                                           PersonActionPayload("start_conversation", hit), "secondary")));
  }

  return ArrayValue(std::move(actions));
}

Value ContactItemActions(const PeopleDiscoveryContactView& contact, const PeopleDiscoveryLabels& labels) {
  std::vector<Value> actions;

  Object message_payload;
  message_payload.set("type", "start_conversation");
  message_payload.set("contact_id", contact.id);
  actions.push_back(ObjectValue(MakeAction(labels.message, Fill(labels.start_chat_with, "name", contact.display_name),
                                           std::move(message_payload), "primary")));

  Object view_payload;
  view_payload.set("type", "show_contact");
  view_payload.set("contact_id", contact.id);
  actions.push_back(ObjectValue(
      MakeAction(labels.view, Fill(labels.show_ids_for, "name", contact.display_name),
                                                  std::move(view_payload), "secondary")));

  return ArrayValue(std::move(actions));
}

void FillPersonRowFields(Object& item, const std::string& display_name, const std::string& nickname,
                         const std::string& person_id, const bool already_contact,
                         const PeopleDiscoveryLabels& labels) {
  std::string title = display_name;
  if (title.empty() && !nickname.empty()) {
    title = "~" + nickname;
  }
  if (title.empty()) {
    title = ShortIdentityId(person_id);
  }
  if (title.empty()) {
    title = labels.unknown_person;
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
    item.set("meta", labels.in_contacts);
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
  if (const Object* local = json.getObject("local")) {
    if (auto display_name = local->getString("display_name")) {
      contact.display_name = *display_name;
    }
  } else if (auto display_name = json.getString("display_name")) {
    contact.display_name = *display_name;
  }
  if (const Object* remote = json.getObject("remote")) {
    if (auto nickname = remote->getString("nickname")) {
      contact.server_nickname = *nickname;
    }
    AppendContactIdsFromJson(*remote, contact.ids);
  } else {
    if (auto server_nickname = json.getString("server_nickname")) {
      contact.server_nickname = *server_nickname;
    }
    AppendContactIdsFromJson(json, contact.ids);
  }
  return contact;
}

std::unordered_set<std::string> CollectKnownIdentities(const std::vector<PeopleDiscoveryContactView>& contacts,
                                                       const PeopleDiscoveryBuildOptions& options) {
  std::unordered_set<std::string> known = options.known_local_identity_values;
  for (const PeopleDiscoveryContactView& contact : contacts) {
    for (const mesh::ContactId& id : contact.ids) {
      if (!id.value.empty()) {
        known.insert(id.value);
      }
    }
  }
  return known;
}

Value RefineSearchFooter(const PeopleDiscoveryLabels& labels) {
  Object refine;
  refine.set("label", labels.refine_label);
  refine.set("message", labels.refine_message);
  return ArrayValue({ObjectValue(std::move(refine))});
}

std::string ShowMoreLabel(const PeopleDiscoveryLabels& labels, const size_t rest) {
  return Fill(labels.show_more_label, "rest", std::to_string(rest));
}

/** "Show more (N left)" reveals `step` more rows of `blocks[block_index]` in place (a local action, no turn). */
Value ShowMoreFooter(const PeopleDiscoveryLabels& labels, const size_t block_index, const size_t step,
                     const size_t rest) {
  Object payload;
  payload.set("type", kShowMoreRowsActionType);
  payload.set("block", static_cast<int64_t>(block_index));
  payload.set("step", static_cast<int64_t>(step));
  return ArrayValue({ObjectValue(
      MakeAction(ShowMoreLabel(labels, rest), labels.show_more_message, std::move(payload), "primary"))});
}

bool IsShowMoreRowsAction(const Value& action_value) {
  const Object* action = asObject(action_value);
  const Object* payload = action ? action->getObject("payload") : nullptr;
  return payload && payload->getString("type").value_or("") == kShowMoreRowsActionType;
}

} // namespace

std::unordered_set<std::string> SelfIdentityValuesFromLocal(const LocalIdentity& identity) {
  std::unordered_set<std::string> out;
  if (!identity.account_id.empty()) {
    out.insert(identity.account_id);
  }
  if (!identity.relay_user_id.empty()) {
    out.insert(identity.relay_user_id);
  }
  if (!identity.peer_id.empty()) {
    out.insert(identity.peer_id);
  }
  return out;
}

bool DirectoryHitMatchesIdentities(const mesh::DirectoryHit& hit,
                                   const std::unordered_set<std::string>& identities) {
  if (identities.empty()) {
    return false;
  }
  if (hit.account_id && identities.count(*hit.account_id) != 0) {
    return true;
  }
  for (const mesh::ContactId& id : hit.ids) {
    if (!id.value.empty() && identities.count(id.value) != 0) {
      return true;
    }
  }
  return false;
}

void SetPersonAvatarFields(Object& item, const std::string& display_name, const std::string& stable_id) {
  SetAvatarFields(item, display_name, stable_id);
}

std::string BuildPeopleDiscoveryBlocksJson(const std::vector<mesh::DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts,
                                           const PeopleDiscoveryBuildOptions& options) {
  const PeopleDiscoveryLabels& labels = options.labels;
  std::vector<Value> blocks;
  const size_t max_visible =
      options.max_visible_items == 0 ? kDefaultMaxVisible : options.max_visible_items;
  const auto known = CollectKnownIdentities(contacts, options);

  std::vector<mesh::DirectoryHit> filtered_hits;
  filtered_hits.reserve(directory_hits.size());
  for (const mesh::DirectoryHit& hit : directory_hits) {
    if (!options.self_identity_values.empty() &&
        DirectoryHitMatchesIdentities(hit, options.self_identity_values)) {
      continue;
    }
    filtered_hits.push_back(hit);
  }

  if (!filtered_hits.empty()) {
    // Up to kMaxListedHits go into the list; the first `visible` show, "show more" reveals the rest in place.
    // Past the cap the list says how many it holds and offers "Refine search" instead of a silent cut.
    const bool truncated = filtered_hits.size() > kMaxListedHits;
    if (truncated) {
      filtered_hits.resize(kMaxListedHits);
    }
    const size_t total = filtered_hits.size();
    const size_t visible = std::min(total, max_visible);

    Object paragraph;
    paragraph.set("type", "paragraph");
    paragraph.set("text", total == 1 ? labels.found_one : Fill(labels.found_many, "count", std::to_string(total)));
    blocks.push_back(ObjectValue(std::move(paragraph)));

    std::vector<Value> items;
    items.reserve(total);
    for (const mesh::DirectoryHit& hit : filtered_hits) {
      const bool already =
          IdentityKnown(hit.ids, hit.account_id, known) ||
          MatchingContactId(hit.ids, hit.account_id, contacts).has_value();
      const auto contact_id = MatchingContactId(hit.ids, hit.account_id, contacts);

      Object item;
      FillPersonRowFields(item, hit.display_name, hit.nickname, PreferPersonId(hit), already, labels);
      item.set("actions", DirectoryHitItemActions(hit, already, contact_id, labels));
      items.push_back(ObjectValue(std::move(item)));
    }

    std::vector<Value> footer_actions;
    if (visible < total) {
      footer_actions = asArray(ShowMoreFooter(labels, blocks.size(), max_visible, total - visible))->elements;
    }
    if (truncated) {
      footer_actions.push_back(asArray(RefineSearchFooter(labels))->elements.front());
    }
    Value footer;
    if (!footer_actions.empty()) {
      footer = ArrayValue(std::move(footer_actions));
    }
    Object list = BuildLongListBlock(labels.results_title, ArrayValue(std::move(items)), std::move(footer));
    list.set("visible", static_cast<int64_t>(visible));
    blocks.push_back(ObjectValue(std::move(list)));
  } else if (!contacts.empty()) {
    const size_t total = contacts.size();
    const size_t visible = std::min(total, max_visible);

    Object paragraph;
    paragraph.set("type", "paragraph");
    if (total == 1) {
      paragraph.set("text", labels.local_one);
    } else if (visible < total) {
      paragraph.set("text", Fill(Fill(labels.local_many_partial, "count", std::to_string(total)), "shown",
                                  std::to_string(visible)));
    } else {
      paragraph.set("text", Fill(labels.local_many, "count", std::to_string(total)));
    }
    blocks.push_back(ObjectValue(std::move(paragraph)));

    std::vector<Value> items;
    items.reserve(visible);
    for (size_t i = 0; i < visible; ++i) {
      const PeopleDiscoveryContactView& contact = contacts[i];
      Object item;
      // Remark first (nickname in small print under it); without a remark the nickname is the title.
      FillPersonRowFields(item, contact.display_name.empty() ? contact.server_nickname : contact.display_name,
                          contact.server_nickname, PreferPersonId(contact), false, labels);
      item.set("actions", ContactItemActions(contact, labels));
      items.push_back(ObjectValue(std::move(item)));
    }
    Value footer;
    if (visible < total) {
      footer = RefineSearchFooter(labels);
    }
    blocks.push_back(
        ObjectValue(BuildLongListBlock(labels.contacts_title, ArrayValue(std::move(items)), std::move(footer))));
  } else {
    Object paragraph;
    paragraph.set("type", "paragraph");
    paragraph.set("text", labels.no_people);
    blocks.push_back(ObjectValue(std::move(paragraph)));
  }

  return BuildBlocksJson(std::move(blocks));
}

bool RevealMoreRows(Object& block, const size_t step, const PeopleDiscoveryLabels& labels) {
  const Array* items = block.getArray("items");
  if (!items || step == 0) {
    return false;
  }
  const size_t rows = items->elements.size();
  const size_t visible = std::min<size_t>(block.getNonNegInt("visible").value_or(rows), rows);
  if (visible >= rows) {
    return false;
  }
  const size_t shown = std::min(rows, visible + step);
  block.set("visible", static_cast<int64_t>(shown));
  if (shown == rows) {
    // Nothing left to reveal; a "refine search" action (list cut at the cap) stays.
    std::vector<Value> rest;
    if (const Array* footer = block.getArray("footer_actions")) {
      for (const Value& action : footer->elements) {
        if (!IsShowMoreRowsAction(action)) {
          rest.push_back(action);
        }
      }
    }
    if (rest.empty()) {
      block.erase("footer_actions");
    } else {
      block.set("footer_actions", ArrayValue(std::move(rest)));
    }
    return true;
  }
  // The action stays (its payload names this block); only the remainder in its label changes.
  if (const Array* footer = block.getArray("footer_actions")) {
    std::vector<Value> actions = footer->elements;
    for (Value& action_value : actions) {
      if (IsShowMoreRowsAction(action_value)) {
        Object action = *asObject(action_value);
        action.set("label", ShowMoreLabel(labels, rows - shown));
        action_value = ObjectValue(std::move(action));
      }
    }
    block.set("footer_actions", ArrayValue(std::move(actions)));
  }
  return true;
}

std::optional<std::string> RevealMoreRowsInBlocksJson(const std::string& blocks_json, const size_t block_index,
                                                      const size_t step, const PeopleDiscoveryLabels& labels) {
  auto doc = TryParseObject(blocks_json);
  const Array* blocks = doc ? doc->getArray("blocks") : nullptr;
  if (!blocks || block_index >= blocks->elements.size()) {
    return std::nullopt;
  }
  const Object* found = asObject(blocks->elements[block_index]);
  if (!found || found->getString("type").value_or("") != "long_list") {
    return std::nullopt;
  }
  Object block = *found;
  if (!RevealMoreRows(block, step, labels)) {
    return std::nullopt;
  }
  std::vector<Value> updated = blocks->elements;
  updated[block_index] = ObjectValue(std::move(block));
  doc->set("blocks", ArrayValue(std::move(updated)));
  return DumpJson(*doc);
}

std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json,
                                                 const PeopleDiscoveryLabels& labels) {
  auto parsed = ParseValue(raw_json);
  if (!parsed) {
    return {};
  }
  const Array* doc = asArray(*parsed);
  if (!doc || doc->elements.empty()) {
    return {};
  }

  std::vector<mesh::DirectoryHit> hits;
  std::vector<PeopleDiscoveryContactView> contacts;
  for (const Value& item_value : doc->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    if (item->contains("hit_id")) {
      hits.push_back(DirectoryHitFromJson(*item));
    } else if (item->contains("id") &&
               (item->contains("display_name") || item->contains("local") || item->contains("remote"))) {
      contacts.push_back(ContactViewFromJson(*item));
    }
  }

  if (hits.empty() && contacts.empty()) {
    return {};
  }
  PeopleDiscoveryBuildOptions options;
  options.labels = labels;
  return BuildPeopleDiscoveryBlocksJson(hits, contacts, options);
}

} // namespace pbr
