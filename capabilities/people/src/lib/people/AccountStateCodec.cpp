#include "lib/people/AccountStateCodec.h"

#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** The fields of a `groups` row and of one of its `members` this build reads (and writes). */
constexpr std::string_view kGroupFields[] = {"group_id",      "title",    "owner",   "roster_epoch",
                                             "title_version", "policy",   "members", "roster_statement",
                                             "left",          "dissolved"};
constexpr std::string_view kMemberFields[] = {"identity", "role", "joined_at", "contact_id"};

} // namespace

void AccountStateCodec::KeepUnknownFields(Object& written, const Object& kept,
                                          const std::span<const std::string_view> known) {
  for (const auto& [key, value] : kept.entries()) {
    if (written.contains(key) || std::find(known.begin(), known.end(), key) != known.end()) {
      continue;
    }
    written.set(key, value);
  }
}

Object AccountStateCodec::GroupToJson(const AccountStateGroup& group, const Object* kept) {
  // The rows the group was read with, by member, so each member's unknown fields go back on it.
  std::map<std::string, const Object*> kept_members;
  if (kept != nullptr) {
    if (const Array* rows = kept->getArray("members")) {
      for (const Value& item : rows->elements) {
        if (const Object* row = asObject(item)) {
          kept_members.emplace(row->getString("identity").value_or(""), row);
        }
      }
    }
  }
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
    if (const auto found = kept_members.find(m.identity); found != kept_members.end()) {
      KeepUnknownFields(row, *found->second, kMemberFields);
    }
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
  if (kept != nullptr) {
    KeepUnknownFields(json, *kept, kGroupFields);
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

Object AccountStateCodec::GrantToJson(const AccountStateGrant& grant) {
  Object row;
  row.set("grant_version", kAccountStateGrantVersion);
  row.set("recipient_account_id", grant.grantor_account_id);
  row.set("sender_account_id", grant.grantee_account_id);
  row.set("issued_at", grant.issued_at);
  row.set("expires_at", grant.expires_at);
  if (!grant.ext_b64.empty()) {
    row.set("ext_b64", grant.ext_b64);
  }
  row.set("signature", grant.signature);
  return row;
}

std::optional<AccountStateGrant> AccountStateCodec::GrantFromJson(const Object& json, const size_t max_field) {
  // Another version is not this build's to read: dropped here (the device holding it publishes it again).
  if (json.getIf<int64_t>("grant_version").value_or(0) != kAccountStateGrantVersion) {
    return std::nullopt;
  }
  AccountStateGrant grant{.grantor_account_id = json.getString("recipient_account_id").value_or(""),
                          .grantee_account_id = json.getString("sender_account_id").value_or(""),
                          .issued_at = json.getIf<int64_t>("issued_at").value_or(0),
                          .expires_at = json.getIf<int64_t>("expires_at").value_or(0),
                          .ext_b64 = json.getString("ext_b64").value_or(""),
                          .signature = json.getString("signature").value_or("")};
  const auto fits = [max_field](const std::string& field) { return !field.empty() && field.size() <= max_field; };
  if (!fits(grant.grantor_account_id) || !fits(grant.grantee_account_id) || !fits(grant.signature) ||
      grant.ext_b64.size() > max_field || grant.issued_at <= 0 || grant.expires_at <= grant.issued_at) {
    return std::nullopt;
  }
  return grant;
}

} // namespace pbr
