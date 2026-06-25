#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "contacts/ContactTypes.h"

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
  Roe<Contact> Upsert(const Contact& contact);
  Roe<std::vector<Contact>> SearchLocal(const std::string& query) const;
  Roe<Contact> AddFromDirectoryHit(const DirectoryHit& hit);
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
