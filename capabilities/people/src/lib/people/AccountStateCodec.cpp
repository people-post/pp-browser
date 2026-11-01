#include "lib/people/AccountStateCodec.h"

#include "common/ValueJson.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

Object AccountStateCodec::GroupToJson(const AccountStateGroup& group) {
  Object json;
  json.set("group_id", group.group_id);
  json.set("title", group.title);
  json.set("owner", group.owner_identity);
  json.setJsonUInt("roster_epoch", group.roster_epoch);
  json.setJsonUInt("title_version", group.title_version);
  json.set("policy", group.policy_json);
  std::vector<Value> members;
  for (const AccountStateGroupMember& m : group.members) {
    Object row;
    row.set("identity", m.identity);
    row.set("role", m.role);
    row.setJsonUInt("joined_at", static_cast<uint64_t>(m.joined_at < 0 ? 0 : m.joined_at));
    row.set("contact_id", m.contact_id);
    members.push_back(ObjectValue(std::move(row)));
  }
  json.set("members", ArrayValue(std::move(members)));
  json.set("roster_statement", group.roster_statement_json);
  if (group.left) {
    json.set("left", true);
  }
  if (group.dissolved) {
    json.set("dissolved", true);
  }
  return json;
}

std::optional<AccountStateGroup> AccountStateCodec::GroupFromJson(const Object& json) {
  AccountStateGroup group;
  group.group_id = json.getString("group_id").value_or("");
  group.owner_identity = json.getString("owner").value_or("");
  if (group.group_id.empty() || group.owner_identity.empty()) {
    return std::nullopt;
  }
  group.title = json.getString("title").value_or("");
  group.roster_epoch = json.getNonNegInt("roster_epoch").value_or(0);
  group.title_version = json.getNonNegInt("title_version").value_or(0);
  group.policy_json = json.getString("policy").value_or("");
  if (const Array* members = json.getArray("members")) {
    for (const Value& item : members->elements) {
      if (const Object* row = asObject(item)) {
        AccountStateGroupMember m;
        m.identity = row->getString("identity").value_or("");
        m.role = row->getString("role").value_or("member");
        m.joined_at = static_cast<int64_t>(row->getNonNegInt("joined_at").value_or(0));
        m.contact_id = row->getString("contact_id").value_or("");
        if (!m.identity.empty()) {
          group.members.push_back(std::move(m));
        }
      }
    }
  }
  group.roster_statement_json = json.getString("roster_statement").value_or("");
  group.left = json.getIf<bool>("left").value_or(false);
  group.dissolved = json.getIf<bool>("dissolved").value_or(false);
  return group;
}

} // namespace pbr
