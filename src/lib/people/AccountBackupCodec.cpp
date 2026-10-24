#include "lib/people/AccountBackupCodec.h"

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
  return contents;
}

} // namespace pbr
