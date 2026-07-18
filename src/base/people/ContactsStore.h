#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "base/people/ContactTypes.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

class ContactsStore : public Module {
public:
  explicit ContactsStore(std::string data_dir);

  Roe<std::vector<Contact>> List() const;
  Roe<std::optional<Contact>> Get(const std::string& contact_id) const;
  /** Exact match on contact.ids[].value; when kind is set, also requires matching kind. */
  Roe<std::optional<Contact>> FindByIdentity(const std::string& identity_value,
                                             std::optional<ContactIdKind> kind = std::nullopt) const;
  Roe<Contact> Upsert(const Contact& contact);
  Roe<bool> Remove(const std::string& contact_id);
  Roe<std::vector<Contact>> SearchLocal(const std::string& query) const;
  Roe<Contact> AddFromDirectoryHit(const DirectoryHit& hit);
  Roe<Contact> AddEmpty();
  void Flush();

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> Save() const;
  std::string StorePath() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::vector<Contact> contacts_;
  mutable bool dirty_ = false;
};

} // namespace pbr
