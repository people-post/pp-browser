#include "lib/people/ContactClock.h"

#include "lib/people/ContactJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include "common/PbrCompat.h"

namespace pbr {

std::string ContactClockAccount(const Contact& contact) {
  for (const mesh::ContactId& id : contact.remote.ids) {
    if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

std::string ContactUserFieldsDigest(const Contact& contact) {
  Object fields;
  fields.set("display_name", contact.local.display_name);
  fields.set("trust", TrustLevelToString(contact.local.trust));
  const std::string text = DumpJson(fields);
  return BytesToHex(Blake2b256(ByteVector(text.begin(), text.end()))).substr(0, 32);
}

ContactClockEntry* ContactClock::Find(const std::string& account_id) {
  auto found = std::find_if(entries_.begin(), entries_.end(),
                            [&](const ContactClockEntry& e) { return e.account_id == account_id; });
  return found == entries_.end() ? nullptr : &*found;
}

bool ContactClock::Observe(const std::vector<Contact>& contacts, const int64_t now_ms) {
  bool changed = false;
  std::map<std::string, std::string> present;  // account → digest
  for (const Contact& contact : contacts) {
    const std::string account = ContactClockAccount(contact);
    if (!account.empty()) {
      present[account] = ContactUserFieldsDigest(contact);
    }
  }
  for (const auto& [account, digest] : present) {
    ContactClockEntry* entry = Find(account);
    if (!entry) {
      entries_.push_back({account, now_ms, true, digest});
      changed = true;
    } else if (!entry->present || entry->digest != digest) {
      *entry = {account, std::max(now_ms, entry->at + 1), true, digest};
      changed = true;
    }
  }
  for (ContactClockEntry& entry : entries_) {
    if (entry.present && !present.contains(entry.account_id)) {
      entry = {entry.account_id, std::max(now_ms, entry.at + 1), false, ""};
      changed = true;
    }
  }
  return changed;
}

ContactClock::Plan ContactClock::MergeRemote(const std::vector<ContactClockEntry>& remote_entries,
                                             const std::vector<Contact>& remote_contacts,
                                             const std::vector<Contact>& local_contacts) {
  Plan plan;
  for (const ContactClockEntry& theirs : remote_entries) {
    ContactClockEntry* ours = Find(theirs.account_id);
    if (ours && ours->at >= theirs.at) {
      continue;  // ours is as new or newer
    }
    const auto local = std::find_if(local_contacts.begin(), local_contacts.end(),
                                    [&](const Contact& c) { return ContactClockAccount(c) == theirs.account_id; });
    if (theirs.present) {
      const auto remote = std::find_if(remote_contacts.begin(), remote_contacts.end(),
                                       [&](const Contact& c) { return ContactClockAccount(c) == theirs.account_id; });
      if (remote == remote_contacts.end()) {
        continue;  // an entry without its contact: nothing to put in
      }
      Contact merged = *remote;
      if (local != local_contacts.end()) {
        merged = *local;  // keep this device's row (id, directory snapshot); take the user's fields
        merged.local.display_name = remote->local.display_name;
        merged.local.trust = remote->local.trust;
        SyncContactMirrors(merged);
      }
      if (local == local_contacts.end() || ContactUserFieldsDigest(*local) != theirs.digest) {
        plan.upsert.push_back(std::move(merged));
      }
    } else if (local != local_contacts.end()) {
      plan.remove_contact_ids.push_back(local->id);
    }
    if (ours) {
      *ours = theirs;
    } else {
      entries_.push_back(theirs);
    }
  }
  return plan;
}

Value ContactClock::EntriesToJson(const std::vector<ContactClockEntry>& entries) {
  std::vector<Value> rows;
  for (const ContactClockEntry& entry : entries) {
    Object row;
    row.set("account_id", entry.account_id);
    row.set("at", entry.at);
    row.set("present", entry.present);
    row.set("digest", entry.digest);
    rows.push_back(ObjectValue(std::move(row)));
  }
  return ArrayValue(std::move(rows));
}

std::vector<ContactClockEntry> ContactClock::EntriesFromJson(const Array& rows) {
  std::vector<ContactClockEntry> entries;
  for (const Value& item : rows.elements) {
    const Object* row = asObject(item);
    if (!row) {
      continue;
    }
    ContactClockEntry entry;
    entry.account_id = row->getString("account_id").value_or("");
    entry.at = row->getIf<int64_t>("at").value_or(0);
    entry.present = row->getIf<bool>("present").value_or(true);
    entry.digest = row->getString("digest").value_or("");
    if (!entry.account_id.empty()) {
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

} // namespace pbr
