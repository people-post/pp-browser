#include "base/messaging/DirectChatTarget.h"

#include "base/messaging/MessagingJson.h"

namespace pbr {

DirectChatTarget DirectChatTargetFromContact(const Contact& contact, ThreadChannel channel) {
  DirectChatTarget target;
  target.channel = channel;
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::RelayUser) {
      target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
      target.peer_identity_value = id.value;
      break;
    }
  }
  return target;
}

} // namespace pbr
