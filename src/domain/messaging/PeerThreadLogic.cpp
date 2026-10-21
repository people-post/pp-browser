#include "domain/messaging/PeerThreadLogic.h"

#include "common/directory/DirectoryJson.h"
#include "common/thread/ThreadChannel.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> ResolveDirectThreadForPeer(IThreadStore& store, const std::string& peer_identity,
                                            const std::string& contact_id, const std::string& title) {
  if (peer_identity.empty()) {
    return Error("Peer identity required");
  }
  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  target.peer_identity_value = peer_identity;
  target.channel = ThreadChannel::E2e;
  auto found = store.FindDirectThread(target);
  if (!found) {
    return found.error();
  }
  if (*found) {
    return (*found)->id;
  }
  target.channel = ThreadChannel::E2ePublic;
  auto thread = store.FindOrCreateDirectThread(target, contact_id, title.empty() ? peer_identity : title);
  if (!thread) {
    return thread.error();
  }
  return thread->id;
}

} // namespace pbr
