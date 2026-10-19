#include "domain/people/ContactReachability.h"

#include "domain/people/ContactIdentity.h"
#include "common/MeshAlias.h"

namespace pbr {

bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  if (PrimaryIdOfKind(contact, mesh::ContactIdKind::Account).empty() &&
      PrimaryIdOfKind(contact, mesh::ContactIdKind::RelayUser).empty() &&
      PrimaryIdOfKind(contact, mesh::ContactIdKind::PeerId).empty()) {
    return false;
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser && !id.value.empty() && relay_configured) {
      return true;
    }
  }
  return !contact.multiaddrs.empty();
}

} // namespace pbr
