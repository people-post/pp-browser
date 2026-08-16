#include "feature/messaging/ContactReachability.h"

#include "base/people/ContactIdentity.h"
#include "base/people/MeshHopPolicy.h"
#include "base/p2p/PeerSessionManager.h"

namespace pbr {

bool IsContactStackDialable(const Contact& contact, const PeerSessionManager* sessions) {
  if (sessions == nullptr) {
    return false;
  }
  const std::string peer_id = PeerIdFromContact(contact);
  if (peer_id.empty()) {
    return false;
  }
  return sessions->IsDialable(peer_id);
}

bool IsContactReachableForMessaging(const Contact& contact, const PeerSessionManager* sessions,
                                    bool relay_configured) {
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
  if (!contact.multiaddrs.empty()) {
    return true;
  }
  return IsContactStackDialable(contact, sessions);
}

} // namespace pbr
