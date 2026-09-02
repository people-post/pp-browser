#include "feature/messaging/ChatBlobRequestUtil.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "base/messaging/AttachmentCache.h"
#include "base/people/ContactTypes.h"
#include "base/people/ContactJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string ResolvePeerRelayId(const Thread& thread, ContactsStore& contacts) {
  if (!thread.participant_contact_ids.empty()) {
    if (auto contact = contacts.Get(thread.participant_contact_ids.front()); contact && *contact) {
      for (const ContactId& id : (*contact)->ids) {
        if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
          return id.value;
        }
      }
    }
  }
  if (thread.peer_identity_kind == ContactIdKindToString(ContactIdKind::Account) && !thread.peer_identity_value.empty()) {
    if (auto by_account = contacts.FindByIdentity(thread.peer_identity_value, ContactIdKind::Account);
        by_account && by_account->has_value()) {
      for (const ContactId& id : (**by_account).ids) {
        if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
          return id.value;
        }
      }
    }
  }
  return {};
}

} // namespace

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
  const std::string peer_relay = ResolvePeerRelayId(thread, contacts);
  if (peer_relay.empty()) {
    return Error("Direct thread missing peer relay route for chat-blob");
  }

  ChatBlobRequest request;
  request.op = op;
  request.requester_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  request.requester_identity_value = local_identity->relay_user_id;
  request.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  request.peer_identity_value = peer_relay;
  request.thread_id = thread_id;
  request.content_hash_hex = AttachmentHashHex(content_hash);
  request.channel = thread.channel;
  return request;
}

} // namespace pbr
