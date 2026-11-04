#pragma once

#include "lib/people/ContactTypes.h"

#include <string>

namespace pbr {

/**
 * The support contact is named by its remote nickname (the support title); local.display_name stays the
 * user's own remark. A fresh contact has no remark.
 */
inline Contact MakeSupportContact(const std::string& contact_id, const std::string& account_id,
                                  const std::string& title) {
  Contact contact;
  contact.id = contact_id;
  contact.local.trust = TrustLevel::Unknown;
  contact.remote.nickname = title;
  contact.remote.ids.push_back({mesh::ContactIdKind::Account, account_id, true});
  SyncContactMirrors(contact);
  return contact;
}

/**
 * Keep the support title current in remote.nickname. A remark equal to the title is the copy older builds
 * wrote there, so it is cleared; any other remark is the user's and stays. Returns true when it changed.
 */
inline bool ApplySupportTitle(Contact& contact, const std::string& title) {
  bool changed = false;
  if (contact.remote.nickname != title) {
    contact.remote.nickname = title;
    changed = true;
  }
  if (contact.local.display_name == title) {
    contact.local.display_name.clear();
    changed = true;
  }
  if (changed) {
    SyncContactMirrors(contact);
  }
  return changed;
}

}  // namespace pbr
