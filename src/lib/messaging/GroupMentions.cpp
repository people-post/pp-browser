#include "lib/messaging/GroupMentions.h"

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

bool IsMember(const std::vector<GroupRosterMember>& roster, const std::string& id) {
  if (id.empty()) {
    return false;
  }
  for (const GroupRosterMember& member : roster) {
    if (member.member_identity == id) {
      return true;
    }
  }
  return false;
}

bool IsOwner(const std::vector<GroupRosterMember>& roster, const std::string& id) {
  if (id.empty()) {
    return false;
  }
  for (const GroupRosterMember& member : roster) {
    if (member.role == MemberRole::Owner && member.member_identity == id) {
      return true;
    }
  }
  return false;
}

std::optional<MentionRange> ReadRange(const Object& json, const bool with_id) {
  MentionRange range;
  if (with_id) {
    range.account_id = json.getString("id").value_or("");
  }
  const auto s = json.getNonNegInt("s");
  const auto e = json.getNonNegInt("e");
  if (!s || !e || *s > UINT32_MAX || *e > UINT32_MAX) {
    return std::nullopt;
  }
  range.start = static_cast<uint32_t>(*s);
  range.end = static_cast<uint32_t>(*e);
  return range;
}

} // namespace

MentionsPayload ParseMentionsPayload(const std::string& payload_json) {
  MentionsPayload out;
  auto json = TryParseObject(payload_json);
  if (!json) {
    return out;
  }
  if (const Array* arr = json->getArray("mentions")) {
    for (const Value& element : arr->elements) {
      const Object* item = asObject(element);
      if (!item) {
        continue;
      }
      if (auto range = ReadRange(*item, true)) {
        out.mentions.push_back(std::move(*range));
      }
    }
  }
  if (const Object* all = json->getObject("all")) {
    out.all = ReadRange(*all, false);
  }
  return out;
}

std::string BuildMentionsPayloadJson(const std::string& text, const MentionsPayload& mentions) {
  Object payload;
  payload.set("text", text);
  std::vector<Value> items;
  items.reserve(mentions.mentions.size());
  for (const MentionRange& range : mentions.mentions) {
    Object item;
    item.set("id", range.account_id);
    item.set("s", static_cast<int64_t>(range.start));
    item.set("e", static_cast<int64_t>(range.end));
    items.push_back(ObjectValue(std::move(item)));
  }
  payload.set("mentions", ArrayValue(std::move(items)));
  if (mentions.all) {
    Object all;
    all.set("s", static_cast<int64_t>(mentions.all->start));
    all.set("e", static_cast<int64_t>(mentions.all->end));
    payload.set("all", ObjectValue(std::move(all)));
  }
  return DumpJson(payload);
}

bool MentionRangeFitsText(const std::string& text, const uint32_t start, const uint32_t end) {
  if (start >= end || end > text.size() || text[start] != '@') {
    return false;
  }
  const auto on_boundary = [&](const uint32_t pos) {
    return pos == text.size() || (static_cast<uint8_t>(text[pos]) & 0xC0) != 0x80;
  };
  return on_boundary(start) && on_boundary(end);
}

std::vector<MentionRange> EligibleMentions(const std::string& payload_json, const std::string& sender_account_id,
                                           const std::vector<GroupRosterMember>& roster) {
  std::vector<MentionRange> out;
  const MentionsPayload payload = ParseMentionsPayload(payload_json);
  for (const MentionRange& range : payload.mentions) {
    if (IsMember(roster, range.account_id)) {
      out.push_back(range);
    }
  }
  if (payload.all && IsOwner(roster, sender_account_id)) {
    out.push_back(*payload.all);
  }
  return out;
}

bool MentionsMe(const std::string& payload_json, const std::string& self_account_id,
                const std::string& sender_account_id, const std::vector<GroupRosterMember>& roster) {
  if (self_account_id.empty()) {
    return false;
  }
  const MentionsPayload payload = ParseMentionsPayload(payload_json);
  if (payload.all && IsOwner(roster, sender_account_id)) {
    return true;
  }
  for (const MentionRange& range : payload.mentions) {
    if (range.account_id == self_account_id && IsMember(roster, self_account_id)) {
      return true;
    }
  }
  return false;
}

} // namespace pbr
