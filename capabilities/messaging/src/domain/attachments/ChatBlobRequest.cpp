#include "domain/attachments/ChatBlobRequest.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/ContactJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

Roe<ChatBlobRequest> BuildChatBlobRequest(const Thread& thread, ContactsStore& /*contacts*/, IdentityStore& identity,
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
  if (local_identity->account_id.empty()) {
    return Error("Local account identity missing");
  }
  // Both parties by Account ID (multi-relay MR001): the thread's peer is its account.
  const std::string& peer_account = thread.peer_identity_value;
  if (peer_account.rfind("account:", 0) != 0) {
    return Error("Direct thread peer has no Account ID for chat-blob");
  }

  ChatBlobRequest request;
  request.op = op;
  request.requester_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  request.requester_identity_value = local_identity->account_id;
  request.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  request.peer_identity_value = peer_account;
  request.thread_id = thread_id;
  request.content_hash_hex = AttachmentHashHex(content_hash);
  request.channel = thread.channel;
  return request;
}

} // namespace pbr
