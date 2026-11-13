#include "lib/people/ContactReachability.h"

#include "lib/people/ContactIdentity.h"
#include "common/MeshAlias.h"

namespace pbr {

bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured, bool account_keys_known) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  const bool has_account = !PrimaryIdOfKind(contact, mesh::ContactIdKind::Account).empty();
  if (!has_account && PrimaryIdOfKind(contact, mesh::ContactIdKind::PeerId).empty()) {
    return false;
  }
  if (has_account && (relay_configured || account_keys_known)) {
    return true;
  }
  return !contact.multiaddrs.empty();
}

} // namespace pbr
