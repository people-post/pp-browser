#include "base/messaging/DirectChatTarget.h"

#include "base/messaging/MessagingJson.h"

namespace pbr {

namespace {

std::string PrimaryIdOfKind(const Contact& contact, const ContactIdKind kind) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == kind && id.primary && !id.value.empty()) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == kind && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

} // namespace

DirectChatTarget DirectChatTargetFromContact(const Contact& contact, ThreadChannel channel) {
  DirectChatTarget target;
  target.channel = channel;
  if (const std::string relay = PrimaryIdOfKind(contact, ContactIdKind::RelayUser); !relay.empty()) {
    target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
    target.peer_identity_value = relay;
  } else if (const std::string peer = PrimaryIdOfKind(contact, ContactIdKind::PeerId); !peer.empty()) {
    target.peer_identity_kind = ContactIdKindToString(ContactIdKind::PeerId);
    target.peer_identity_value = peer;
  }
  return target;
}

} // namespace pbr
