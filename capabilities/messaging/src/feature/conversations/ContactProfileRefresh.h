#pragma once

#include "lib/people/ContactsStore.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace pbr {

/**
 * Keeps a contact's nickname and profile photo current (B142): a contact whose last directory answer is older
 * than `kStaleMs` is looked up again by its Account ID and only `remote.nickname`, `remote.icon` and
 * `remote.fetched_at` are updated (ContactsStore::ApplyProfileRefresh). Blocking: call it off the UI thread.
 */
class ContactProfileRefresh {
public:
  /** A directory answer older than this is asked for again. */
  static constexpr int64_t kStaleMs = 6LL * 60LL * 60LL * 1000LL;

  struct Result {
    /** A directory lookup was made (the caller spaces these out). */
    bool looked_up = false;
    /** The contact as stored after the update; empty when nothing was applied. */
    std::optional<Contact> updated;
  };

  ContactProfileRefresh(ContactsStore& contacts, mesh::IDirectoryClient& directory);

  /** True when the contact is known, has an Account ID and its last directory answer is stale at `now_ms`. */
  bool NeedsRefresh(const std::string& contact_id, int64_t now_ms) const;
  /** Looks the contact up and applies the answer when it is stale; a fresh or unknown contact costs no lookup. */
  Result RefreshIfStale(const std::string& contact_id, int64_t now_ms);

private:
  ContactsStore& contacts_;
  mesh::IDirectoryClient& directory_;
  std::mutex mutex_;
  std::set<std::string> in_flight_; // one lookup per contact at a time (detail and chat opened together)
};

} // namespace pbr
