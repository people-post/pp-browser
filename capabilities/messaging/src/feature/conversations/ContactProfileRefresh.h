#pragma once

#include "lib/people/ContactsStore.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace pbr {

/**
 * Keeps a contact's nickname and profile photo current (B142): a contact whose last directory answer is older
 * than `kStaleMs` is looked up again by its Account ID (else its relay id) and only `remote.nickname`,
 * `remote.icon` and `remote.fetched_at` are updated (ContactsStore::ApplyProfileRefresh). A contact without a
 * relay id (added from a QR code) also takes the relay id from the answer, and nothing else (no devices). The
 * scan itself asks once with `force`. A lookup is not repeated within `kStaleMs`, answered or not (kept in
 * memory, from the moment it starts). Blocking: call it off the UI thread.
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

  /**
   * True when the contact is known, has an Account ID or a relay id, and a lookup is due at `now_ms`: its last
   * answer is stale, or `force` (a fresh QR scan, which has no relay id yet) skips that check. A lookup made
   * lately is never repeated, forced or not.
   */
  bool NeedsRefresh(const std::string& contact_id, int64_t now_ms, bool force = false) const;
  /** Looks the contact up and applies the answer when it is due; a fresh or unknown contact costs no lookup. */
  Result RefreshIfStale(const std::string& contact_id, int64_t now_ms, bool force = false);

private:
  bool Due(const Contact& contact, int64_t now_ms, bool force) const;

  ContactsStore& contacts_;
  mesh::IDirectoryClient& directory_;
  mutable std::mutex mutex_;
  std::map<std::string, int64_t> last_attempt_ms_; // contact id -> when it was last looked up
  std::set<std::string> in_flight_;                // one lookup per contact at a time (detail and chat opened together)
};

} // namespace pbr
