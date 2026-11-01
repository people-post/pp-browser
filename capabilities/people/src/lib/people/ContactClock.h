#pragma once

#include "lib/people/ContactTypes.h"

#include "common/Value.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * One contact's sync register (multi-device M023 §7): whether the account is in the address book, what the user set
 * on it (a digest of the remark and trust), and when that last changed on any device. The newest entry wins; a
 * removal is an entry with `present = false` (a tombstone), so it carries to the other devices.
 */
struct ContactClockEntry {
  std::string account_id;
  int64_t at = 0;
  bool present = true;
  /** BLAKE2b of the user's own fields (remark, trust) — directory refreshes do not count as edits. */
  std::string digest;

  bool operator==(const ContactClockEntry&) const = default;
};

/** The digest ContactClockEntry keeps for a contact. */
std::string ContactUserFieldsDigest(const Contact& contact);
/** The contact's Account ID; empty for a contact without one (not synced). */
std::string ContactClockAccount(const Contact& contact);

/**
 * This device's registers, and the two operations that keep them: noticing local edits, and taking the winners of a
 * remote set (the locker's). Not thread-safe (the caller serializes).
 */
class ContactClock {
public:
  /**
   * Compare the address book with the registers: an account added, removed or edited since gets a new entry at
   * `now_ms`. True when anything changed.
   */
  bool Observe(const std::vector<Contact>& contacts, int64_t now_ms);

  /** What a remote set changes here: contacts to put in (remote copies) and contact ids to take out. */
  struct Plan {
    std::vector<Contact> upsert;
    std::vector<std::string> remove_contact_ids;
    bool empty() const { return upsert.empty() && remove_contact_ids.empty(); }
  };
  /**
   * Take each remote entry newer than ours (ties: ours): our register becomes it, and the plan says what to change in
   * the address book so that a later Observe sees nothing new. `remote_contacts` are the remote side's present
   * contacts; `local_contacts` the address book now.
   */
  Plan MergeRemote(const std::vector<ContactClockEntry>& remote_entries, const std::vector<Contact>& remote_contacts,
                   const std::vector<Contact>& local_contacts);

  const std::vector<ContactClockEntry>& Entries() const { return entries_; }

  Value ToJson() const { return EntriesToJson(entries_); }
  void LoadJson(const Value& json) {
    const Array* rows = asArray(json);
    entries_ = rows ? EntriesFromJson(*rows) : std::vector<ContactClockEntry>{};
  }

  void LoadEntries(std::vector<ContactClockEntry> entries) { entries_ = std::move(entries); }

  static Value EntriesToJson(const std::vector<ContactClockEntry>& entries);
  static std::vector<ContactClockEntry> EntriesFromJson(const Array& rows);

private:
  ContactClockEntry* Find(const std::string& account_id);

  std::vector<ContactClockEntry> entries_;
};

} // namespace pbr
