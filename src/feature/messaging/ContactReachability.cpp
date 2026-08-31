#include "feature/messaging/ContactReachability.h"

#include "base/people/ContactIdentity.h"

namespace pbr {

bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  if (PrimaryIdOfKind(contact, ContactIdKind::Account).empty() &&
      PrimaryIdOfKind(contact, ContactIdKind::RelayUser).empty() &&
      PrimaryIdOfKind(contact, ContactIdKind::PeerId).empty()) {
    return false;
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser && !id.value.empty() && relay_configured) {
      return true;
    }
  }
  return !contact.multiaddrs.empty();
}

} // namespace pbr
