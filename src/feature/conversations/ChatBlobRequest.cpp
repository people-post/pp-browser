#include "feature/conversations/ChatBlobRequest.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/ContactJson.h"
#include "lib/people/PeerBriefRoute.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {


Roe<ChatBlobRequest> BuildChatBlobRequest(const Thread& thread, ContactsStore& contacts, IdentityStore& identity,
                                          const ChatBlobOp op, const std::string& thread_id,
                                          const std::vector<uint8_t>& content_hash) {
  if (thread.kind != ThreadKind::Direct || !ThreadChannelIsE2e(thread.channel)) {
    return Error("Peer blob transfer requires E2E direct thread");
  }
  if (content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash");
  }
  auto local_identity = identity.Get();
  if (!local_identity) {
    return local_identity.error();
  }
  if (local_identity->relay_user_id.empty()) {
    return Error("Local relay identity missing");
  }
  const std::string peer_relay = ContactRelayRoute(thread, contacts).value_or(std::string());
  if (peer_relay.empty()) {
    return Error("Direct thread missing peer relay route for chat-blob");
  }

  ChatBlobRequest request;
  request.op = op;
  request.requester_identity_kind = ContactIdKindToString(mesh::ContactIdKind::RelayUser);
  request.requester_identity_value = local_identity->relay_user_id;
  request.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::RelayUser);
  request.peer_identity_value = peer_relay;
  request.thread_id = thread_id;
  request.content_hash_hex = AttachmentHashHex(content_hash);
  request.channel = thread.channel;
  return request;
}

} // namespace pbr
