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

/** Keep the support title current in remote.nickname; the remark is the user's and stays. True when it changed. */
inline bool ApplySupportTitle(Contact& contact, const std::string& title) {
  if (contact.remote.nickname == title) {
    return false;
  }
  contact.remote.nickname = title;
  SyncContactMirrors(contact);
  return true;
}

} // namespace pbr
