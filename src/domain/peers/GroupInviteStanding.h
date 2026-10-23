#pragma once

#include "lib/messaging/GroupInviteGate.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/ContactTypes.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** The invite gate's view of an inviter, from the contacts (messaging does not include people). */
inline GroupInviteGate::StandingFn InviterStandingFromContacts(ContactsStore& contacts) {
  return [&contacts](const std::string& identity) {
    auto contact = contacts.FindByIdentity(identity);
    if (!contact || !*contact) {
      return GroupInviteGate::InviterStanding::Unknown;
    }
    return (*contact)->trust == TrustLevel::Blocked ? GroupInviteGate::InviterStanding::Blocked
                                                    : GroupInviteGate::InviterStanding::Contact;
  };
}

} // namespace pbr
