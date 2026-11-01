#include "lib/people/ContactReachability.h"

#include "lib/people/ContactIdentity.h"
#include "common/MeshAlias.h"

namespace pbr {

bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured, bool account_keys_known) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  if (PrimaryIdOfKind(contact, mesh::ContactIdKind::Account).empty() &&
      PrimaryIdOfKind(contact, mesh::ContactIdKind::RelayUser).empty() &&
      PrimaryIdOfKind(contact, mesh::ContactIdKind::PeerId).empty()) {
    return false;
  }
  if (account_keys_known && !PrimaryIdOfKind(contact, mesh::ContactIdKind::Account).empty()) {
    return true;
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser && !id.value.empty() && relay_configured) {
      return true;
    }
  }
  return !contact.multiaddrs.empty();
}

} // namespace pbr
