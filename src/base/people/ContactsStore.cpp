#include "base/people/ContactsStore.h"

#include "base/data/AtomicFileWrite.h"
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
  std::ifstream in(StorePath());
  if (in) {
    const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    if (!root.is_discarded() && root.contains("contacts") && root["contacts"].is_array()) {
      for (const auto& item : root["contacts"]) {
        contacts_.push_back(ContactFromJson(item));
      }
    }
  }

  loaded_ = true;
  return {};
}

Roe<void> ContactsStore::Save() const {
  nlohmann::json contacts = nlohmann::json::array();
  for (const Contact& contact : contacts_) {
    contacts.push_back(ContactToJson(contact));
  }
  const nlohmann::json root = {{"contacts", std::move(contacts)}};

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

Roe<Contact> ContactsStore::Upsert(const Contact& contact) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  for (Contact& existing : contacts_) {
    if (existing.id == contact.id) {
      existing = contact;
      dirty_ = true;
      if (Save()) {
        return contact;
      }
      return Error("Failed to save contacts");
    }
  }

  contacts_.push_back(contact);
  dirty_ = true;
  if (Save()) {
    return contact;
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
  Contact contact;
  contact.id = util::GenerateUuid();
  contact.display_name = hit.display_name.empty() ? hit.nickname : hit.display_name;
  contact.server_nickname = hit.nickname;
  contact.ids = hit.ids;
  contact.multiaddrs = hit.multiaddrs;
  contact.trust = TrustLevel::Unknown;
  return Upsert(contact);
}

Roe<Contact> ContactsStore::AddEmpty() {
  Contact contact;
  contact.id = util::GenerateUuid();
  contact.trust = TrustLevel::Unknown;
  return Upsert(contact);
}

} // namespace pbr
