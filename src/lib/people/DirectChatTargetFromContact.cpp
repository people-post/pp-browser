#include "lib/people/DirectChatTargetFromContact.h"

#include "lib/people/ContactIdentity.h"
#include "common/directory/DirectoryJson.h"
#include "common/MeshAlias.h"

namespace pbr {

DirectChatTarget DirectChatTargetFromContact(const Contact& contact, ThreadChannel channel) {
  DirectChatTarget target;
  target.channel = channel;
  if (const std::string account = PrimaryIdOfKind(contact, mesh::ContactIdKind::Account); !account.empty()) {
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = account;
  }
  return target;
}

} // namespace pbr
