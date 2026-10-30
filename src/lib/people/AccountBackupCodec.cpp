#include "lib/people/AccountBackupCodec.h"

#include <optional>

#include "foundation/crypto/CryptoUtil.h"
#include "lib/people/ContactJson.h"
#include "common/ValueJson.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {
constexpr const char kSchema[] = "pp-browser-account-backup";
} // namespace

Roe<std::vector<uint8_t>> AccountBackupCodec::Serialize(const AccountBackupV2& backup) {
  if (backup.profile_id.empty() || backup.vault.empty() || backup.sealed.empty()) {
    return Error("Account backup: profile id, vault and sealed contents are required");
  }
  Object out;
  out.set("schema", kSchema);
  out.set("version", static_cast<int64_t>(kSchemaVersion));
  out.set("updated_at_ms", backup.updated_at_ms);
  out.set("nickname", backup.nickname);
  out.set("profile_id", backup.profile_id);
  out.set("vault_b64", Base64Encode(backup.vault));
  out.set("sealed_b64", Base64Encode(backup.sealed));
  const std::string json = DumpJson(out);
  return std::vector<uint8_t>(json.begin(), json.end());
}

Roe<AccountBackupV2> AccountBackupCodec::Parse(const std::vector<uint8_t>& bytes) {
  auto root = ParseObject(std::string(bytes.begin(), bytes.end()));
  if (!root) {
    return Error("Account backup: not a JSON object");
  }
  if (ObjectString(*root, "schema").value_or("") != kSchema) {
    return Error("Account backup: unknown schema");
  }
  if (ObjectInt64(*root, "version").value_or(0) != kSchemaVersion) {
    return Error("Account backup: unsupported version");
  }
  AccountBackupV2 backup;
  backup.updated_at_ms = ObjectInt64(*root, "updated_at_ms").value_or(0);
  backup.nickname = ObjectString(*root, "nickname").value_or("");
  backup.profile_id = ObjectString(*root, "profile_id").value_or("");
  auto vault = Base64Decode(ObjectString(*root, "vault_b64").value_or(""));
  auto sealed = Base64Decode(ObjectString(*root, "sealed_b64").value_or(""));
  if (backup.profile_id.empty() || !vault || vault->empty() || !sealed || sealed->empty()) {
    return Error("Account backup: profile id, vault or sealed contents missing");
  }
  backup.vault = std::move(*vault);
  backup.sealed = std::move(*sealed);
  return backup;
}

Object AccountBackupCodec::GroupToJson(const AccountBackupGroup& group) {
  Object json;
  json.set("group_id", group.group_id);
  json.set("title", group.title);
  json.set("owner", group.owner_identity);
  json.setJsonUInt("roster_epoch", group.roster_epoch);
  json.setJsonUInt("title_version", group.title_version);
  json.set("policy", group.policy_json);
  std::vector<Value> members;
  for (const AccountBackupGroupMember& m : group.members) {
    Object row;
    row.set("identity", m.identity);
    row.set("role", m.role);
    row.setJsonUInt("joined_at", static_cast<uint64_t>(m.joined_at < 0 ? 0 : m.joined_at));
    row.set("contact_id", m.contact_id);
    members.push_back(ObjectValue(std::move(row)));
  }
  json.set("members", ArrayValue(std::move(members)));
  json.set("roster_statement", group.roster_statement_json);
  return json;
}

std::optional<AccountBackupGroup> AccountBackupCodec::GroupFromJson(const Object& json) {
  AccountBackupGroup group;
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
        AccountBackupGroupMember m;
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
  return group;
}

Roe<std::vector<uint8_t>> AccountBackupCodec::SerializeContents(const AccountBackupContents& contents) {
  auto bundle = ParseObject(contents.link_bundle_json);
  if (!bundle) {
    return Error("Account backup: link bundle is not a JSON object");
  }
  std::vector<Value> contacts;
  contacts.reserve(contents.contacts.size());
  for (const Contact& contact : contents.contacts) {
    contacts.push_back(ObjectValue(ContactToJson(contact)));
  }
  Object out;
  out.set("link_bundle", *bundle);
  out.set("contacts", ArrayValue(std::move(contacts)));
  if (!contents.extras.groups.empty()) {
    std::vector<Value> groups;
    for (const AccountBackupGroup& group : contents.extras.groups) {
      groups.push_back(ObjectValue(GroupToJson(group)));
    }
    out.set("groups", ArrayValue(std::move(groups)));
  }
  if (!contents.extras.card_floors.empty()) {
    std::vector<Value> floors;
    for (const AccountBackupCardFloor& floor : contents.extras.card_floors) {
      Object row;
      row.set("account_id", floor.account_id);
      row.set("issued_at", floor.issued_at);
      floors.push_back(ObjectValue(std::move(row)));
    }
    out.set("card_floors", ArrayValue(std::move(floors)));
  }
  const std::string json = DumpJson(out);
  return std::vector<uint8_t>(json.begin(), json.end());
}

Roe<AccountBackupContents> AccountBackupCodec::ParseContents(const std::vector<uint8_t>& bytes) {
  auto root = ParseObject(std::string(bytes.begin(), bytes.end()));
  if (!root) {
    return Error("Account backup: contents are not a JSON object");
  }
  const Object* bundle = root->getObject("link_bundle");
  if (bundle == nullptr) {
    return Error("Account backup: link bundle missing");
  }
  AccountBackupContents contents;
  contents.link_bundle_json = DumpJson(*bundle);
  if (const Array* contacts = root->getArray("contacts")) {
    for (const Value& item : contacts->elements) {
      if (const Object* contact = asObject(item)) {
        contents.contacts.push_back(ContactFromJson(*contact));
      }
    }
  }
  if (const Array* groups = root->getArray("groups")) {
    for (const Value& item : groups->elements) {
      if (const Object* group = asObject(item)) {
        if (auto parsed = GroupFromJson(*group)) {
          contents.extras.groups.push_back(std::move(*parsed));
        }
      }
    }
  }
  if (const Array* floors = root->getArray("card_floors")) {
    for (const Value& item : floors->elements) {
      const Object* row = asObject(item);
      if (row == nullptr) {
        continue;
      }
      AccountBackupCardFloor floor;
      floor.account_id = row->getString("account_id").value_or("");
      floor.issued_at = row->getIf<int64_t>("issued_at").value_or(0);
      if (!floor.account_id.empty() && floor.issued_at > 0) {
        contents.extras.card_floors.push_back(std::move(floor));
      }
    }
  }
  return contents;
}

} // namespace pbr
