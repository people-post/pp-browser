#include "base/people/DirectChatTargetFromContact.h"

#include "base/people/ContactIdentity.h"
#include "common/directory/DirectoryJson.h"

namespace pbr {

DirectChatTarget DirectChatTargetFromContact(const Contact& contact, ThreadChannel channel) {
  DirectChatTarget target;
  target.channel = channel;
  if (const std::string account = PrimaryIdOfKind(contact, ContactIdKind::Account); !account.empty()) {
    target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
    target.peer_identity_value = account;
  }
  return target;
}

} // namespace pbr
