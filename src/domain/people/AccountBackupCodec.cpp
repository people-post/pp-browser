#include "domain/people/AccountBackupCodec.h"

#include "domain/people/ContactJson.h"
#include "common/ValueJson.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {
constexpr const char kSchema[] = "pp-browser-account-backup";
} // namespace

Roe<std::vector<uint8_t>> AccountBackupCodec::Serialize(const AccountBackupV1& backup) {
  auto bundle = ParseObject(backup.link_bundle_json);
  if (!bundle) {
    return Error("Account backup: link bundle is not a JSON object");
  }
  std::vector<Value> contacts;
  contacts.reserve(backup.contacts.size());
  for (const Contact& contact : backup.contacts) {
    contacts.push_back(ObjectValue(ContactToJson(contact)));
  }
  Object out;
  out.set("schema", kSchema);
  out.set("version", static_cast<int64_t>(kSchemaVersion));
  out.set("updated_at_ms", backup.updated_at_ms);
  out.set("link_bundle", *bundle);
  out.set("contacts", ArrayValue(std::move(contacts)));
  const std::string json = DumpJson(out);
  return std::vector<uint8_t>(json.begin(), json.end());
}

Roe<AccountBackupV1> AccountBackupCodec::Parse(const std::vector<uint8_t>& bytes) {
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
  const Object* bundle = root->getObject("link_bundle");
  if (bundle == nullptr) {
    return Error("Account backup: link bundle missing");
  }
  AccountBackupV1 backup;
  backup.updated_at_ms = ObjectInt64(*root, "updated_at_ms").value_or(0);
  backup.link_bundle_json = DumpJson(*bundle);
  if (const Array* contacts = root->getArray("contacts")) {
    for (const Value& item : contacts->elements) {
      if (const Object* contact = asObject(item)) {
        backup.contacts.push_back(ContactFromJson(*contact));
      }
    }
  }
  return backup;
}

} // namespace pbr
