#pragma once

#include "common/directory/IContactTrustAccess.h"
#include "lib/people/IContactBook.h"
#include "common/Error.h"
#include "common/Module.h"
#include "lib/people/ContactTypes.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class ContactsStore : public Module, public IContactBook, public IContactTrustAccess {
public:
  /**
   * Nested local/remote contact rows (+ empty overrides). 2 is stepped up on load: its `relay_user` ids are dropped
   * (the relay addresses a person by the Account ID, multi-relay MR001). A file of another version (older than 2,
   * newer, or none) is refused: pre-release, no leftover contacts file is carried over (COMPATIBILITY.md § On-disk).
   */
  static constexpr int kSchemaVersion = 3;
  static constexpr int kFloorSchemaVersion = 2;

  explicit ContactsStore(std::string data_dir);

  Roe<std::vector<Contact>> List() const override;
  Roe<std::optional<Contact>> Get(const std::string& contact_id) const;
  /** Exact match on contact.ids[].value; when kind is set, also requires matching kind. */
  Roe<std::optional<Contact>> FindByIdentity(const std::string& identity_value,
                                             std::optional<mesh::ContactIdKind> kind = std::nullopt) const override;
  Roe<Contact> Upsert(const Contact& contact) override;
  Roe<bool> Remove(const std::string& contact_id);
  Roe<std::vector<Contact>> SearchLocal(const std::string& query) const;
  Roe<Contact> AddFromDirectoryHit(const mesh::DirectoryHit& hit);
  /**
   * Refresh remote snapshot from a directory hit; preserves local annotations. A contact with an Account ID
   * only takes a hit for that same account (else "Directory hit is for another account", nothing written);
   * `requested_account_id` is the account a by-account lookup asked for, taken as the hit's when it names none.
   * For such a contact what the hit leaves out stays: the relay id, the nickname, and the devices (PeerId ids,
   * endpoints, multiaddrs) as one group. A contact without an Account ID is overwritten as a whole.
   */
  Roe<Contact> ApplyRemoteSnapshot(const std::string& contact_id, const mesh::DirectoryHit& hit, int64_t fetched_at_ms,
                                   const std::optional<std::string>& requested_account_id = std::nullopt);
  /**
   * Profile refresh from a directory hit of the SAME account: only `remote.nickname` (when the hit's is not
   * empty), `remote.icon` and `remote.fetched_at` change; ids, keys and endpoints never do. A hit for another
   * account (or without one) is refused and nothing is written; `requested_account_id` is the account a
   * by-account lookup asked for, taken as the hit's when it names none.
   */
  Roe<Contact> ApplyProfileRefresh(const std::string& contact_id, const mesh::DirectoryHit& hit, int64_t fetched_at_ms,
                                   const std::optional<std::string>& requested_account_id = std::nullopt);
  Roe<Contact> AddEmpty();
  void Flush();
  /**
   * Called after any change that succeeded (add, update, remove, block, remote refresh), on the
   * caller's thread and outside the store's lock.
   */
  void SetOnChanged(std::function<void()> on_changed);

  Roe<bool> IsAccountBlocked(const std::string& account_id) const override;
  Roe<void> BlockAccountIfPresent(const std::string& account_id) override;

private:
  Roe<Contact> UpsertUnnotified(const Contact& contact);
  Roe<bool> RemoveUnnotified(const std::string& contact_id);
  Roe<Contact> AddFromDirectoryHitUnnotified(const mesh::DirectoryHit& hit);
  Roe<Contact> ApplyRemoteSnapshotUnnotified(const std::string& contact_id, const mesh::DirectoryHit& hit,
                                             int64_t fetched_at_ms,
                                             const std::optional<std::string>& requested_account_id);
  Roe<Contact> ApplyProfileRefreshUnnotified(const std::string& contact_id, const mesh::DirectoryHit& hit,
                                             int64_t fetched_at_ms,
                                             const std::optional<std::string>& requested_account_id);
  Roe<Contact> AddEmptyUnnotified();
  Roe<void> BlockAccountIfPresentUnnotified(const std::string& account_id);
  void NotifyChanged() const;
  Roe<void> EnsureLoaded() const;
  Roe<void> Save() const;
  std::string StorePath() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::vector<Contact> contacts_;
  mutable bool dirty_ = false;
  mutable std::mutex observer_mu_;
  std::function<void()> on_changed_;
};

} // namespace pbr
