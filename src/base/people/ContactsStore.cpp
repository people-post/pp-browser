#include "base/people/ContactsStore.h"

#include "base/data/AtomicFileWrite.h"
#include "base/data/SchemaVersion.h"
#include "base/people/ContactJson.h"
#include "common/Utilities.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string ToLowerCopy(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool ContainsInsensitive(const std::string& haystack, const std::string& needle) {
  return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::string::npos;
}

} // namespace

ContactsStore::ContactsStore(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("ContactsStore");
}

std::string ContactsStore::StorePath() const {
  return (std::filesystem::path(data_dir_) / "contacts.json").string();
}

Roe<void> ContactsStore::EnsureLoaded() const {
  if (loaded_) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  contacts_.clear();
  bool needs_rewrite = false;
  {
    // Close the read handle before Save() — Windows cannot rename over an open file.
    std::ifstream in(StorePath());
    if (in) {
      nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
      if (root.is_discarded() || !root.is_object()) {
        return Error("Invalid contacts.json");
      }

      int version = 0;
      if (root.contains("schema_version")) {
        if (!root["schema_version"].is_number_integer()) {
          return Error("Invalid schema_version in contacts.json");
        }
        version = root["schema_version"].get<int>();
        if (auto checked = SchemaVersion::Validate(root, kSchemaVersion, "contacts.json"); !checked) {
          return checked.error();
        }
      }

      if (root.contains("contacts") && root["contacts"].is_array()) {
        for (const auto& item : root["contacts"]) {
          contacts_.push_back(ContactFromJson(item));
        }
      }

      if (version < kSchemaVersion) {
        needs_rewrite = true;
      }
    }
  }

  loaded_ = true;
  if (needs_rewrite) {
    dirty_ = true;
    if (auto saved = Save(); !saved) {
      return saved.error();
    }
    dirty_ = false;
  }
  return {};
}

Roe<void> ContactsStore::Save() const {
  nlohmann::json contacts = nlohmann::json::array();
  for (const Contact& contact : contacts_) {
    contacts.push_back(ContactToJson(contact));
  }
  const nlohmann::json root = {{"schema_version", kSchemaVersion}, {"contacts", std::move(contacts)}};

  return AtomicFileWrite::Write(StorePath(), root.dump(2));
}

void ContactsStore::Flush() {
  std::lock_guard lock(mutex_);
  if (!dirty_) {
    return;
  }
  if (Save()) {
    dirty_ = false;
  }
}

Roe<std::vector<Contact>> ContactsStore::List() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  return contacts_;
}

Roe<std::optional<Contact>> ContactsStore::Get(const std::string& contact_id) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  for (const Contact& contact : contacts_) {
    if (contact.id == contact_id) {
      return Roe<std::optional<Contact>>(contact);
    }
  }
  return Roe<std::optional<Contact>>(std::optional<Contact>{});
}

Roe<std::optional<Contact>> ContactsStore::FindByIdentity(const std::string& identity_value,
                                                          const std::optional<ContactIdKind> kind) const {
  if (identity_value.empty()) {
    return Roe<std::optional<Contact>>(std::optional<Contact>{});
  }
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  for (const Contact& contact : contacts_) {
    for (const ContactId& id : contact.ids) {
      if (id.value != identity_value) {
        continue;
      }
      if (kind && id.kind != *kind) {
        continue;
      }
      return Roe<std::optional<Contact>>(contact);
    }
  }
  return Roe<std::optional<Contact>>(std::optional<Contact>{});
}

Roe<Contact> ContactsStore::Upsert(const Contact& contact) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  Contact stored = contact;
  PromoteFlatFieldsToNested(stored);
  SyncContactMirrors(stored);

  for (Contact& existing : contacts_) {
    if (existing.id == stored.id) {
      existing = stored;
      dirty_ = true;
      if (Save()) {
        return stored;
      }
      return Error("Failed to save contacts");
    }
  }

  contacts_.push_back(stored);
  dirty_ = true;
  if (Save()) {
    return stored;
  }
  return Error("Failed to save contacts");
}

Roe<bool> ContactsStore::Remove(const std::string& contact_id) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  const auto it = std::find_if(contacts_.begin(), contacts_.end(),
                               [&](const Contact& contact) { return contact.id == contact_id; });
  if (it == contacts_.end()) {
    return false;
  }

  contacts_.erase(it);
  dirty_ = true;
  if (Save()) {
    return true;
  }
  return Error("Failed to save contacts");
}

Roe<std::vector<Contact>> ContactsStore::SearchLocal(const std::string& query) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  if (query.empty()) {
    return contacts_;
  }

  std::vector<Contact> out;
  for (const Contact& contact : contacts_) {
    if (ContainsInsensitive(contact.display_name, query) || ContainsInsensitive(contact.server_nickname, query)) {
      out.push_back(contact);
      continue;
    }
    for (const ContactId& id : contact.ids) {
      if (ContainsInsensitive(id.value, query)) {
        out.push_back(contact);
        break;
      }
    }
  }
  return out;
}

Roe<Contact> ContactsStore::AddFromDirectoryHit(const DirectoryHit& hit) {
  std::string account_id;
  if (hit.account_id && !hit.account_id->empty()) {
    account_id = *hit.account_id;
  }
  if (account_id.empty()) {
    for (const ContactId& id : hit.ids) {
      if (id.kind == ContactIdKind::Account && !id.value.empty()) {
        account_id = id.value;
        if (id.primary) {
          break;
        }
      }
    }
  }
  if (account_id.empty()) {
    return Error("Directory hit missing Account ID");
  }

  auto existing = FindByIdentity(account_id, ContactIdKind::Account);
  if (!existing) {
    return existing.error();
  }
  if (existing->has_value()) {
    return ApplyRemoteSnapshot((*existing)->id, hit, util::NowUnixMs());
  }

  Contact contact;
  contact.id = util::GenerateUuid();
  const std::string seed = hit.display_name.empty() ? hit.nickname : hit.display_name;
  contact.local.display_name = seed;
  contact.local.trust = TrustLevel::Unknown;
  contact.remote.nickname = hit.nickname;
  contact.remote.ids = hit.ids;
  bool has_account = false;
  for (ContactId& id : contact.remote.ids) {
    if (id.kind == ContactIdKind::Account && id.value == account_id) {
      id.primary = true;
      has_account = true;
    } else {
      id.primary = false;
    }
  }
  if (!has_account) {
    contact.remote.ids.insert(contact.remote.ids.begin(), {ContactIdKind::Account, account_id, true});
  }
  contact.remote.multiaddrs = hit.multiaddrs;
  contact.remote.fetched_at = util::NowUnixMs();
  SyncContactMirrors(contact);
  return Upsert(contact);
}

Roe<Contact> ContactsStore::ApplyRemoteSnapshot(const std::string& contact_id, const DirectoryHit& hit,
                                                const int64_t fetched_at_ms) {
  auto loaded = Get(contact_id);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return Error("Contact not found");
  }
  Contact contact = **loaded;
  contact.remote.nickname = hit.nickname;
  contact.remote.ids = hit.ids;
  contact.remote.multiaddrs = hit.multiaddrs;
  contact.remote.fetched_at = fetched_at_ms > 0 ? fetched_at_ms : util::NowUnixMs();
  SyncContactMirrors(contact);
  return Upsert(contact);
}

Roe<Contact> ContactsStore::AddEmpty() {
  Contact contact;
  contact.id = util::GenerateUuid();
  contact.local.trust = TrustLevel::Unknown;
  SyncContactMirrors(contact);
  return Upsert(contact);
}

} // namespace pbr
