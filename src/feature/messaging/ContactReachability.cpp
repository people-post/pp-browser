#include "feature/messaging/ContactReachability.h"

#include "base/messaging/DirectChatTarget.h"
#include "base/people/MeshHopPolicy.h"
#include "libp2p/integration/host/PeerSessionManager.h"

namespace pbr {

bool ContactHasDialIdentity(const Contact& contact) {
  if (contact.trust == TrustLevel::Blocked) {
    return false;
  }
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  return !target.peer_identity_value.empty();
}

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
  if (!ContactHasDialIdentity(contact)) {
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
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_kind == ContactIdKindToString(ContactIdKind::PeerId)) {
    return IsContactStackDialable(contact, sessions);
  }
  return false;
}

} // namespace pbr
