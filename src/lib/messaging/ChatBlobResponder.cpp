#include "lib/messaging/ChatBlobResponder.h"

#include "lib/messaging/PeerRequestAuth.h"
#include "common/directory/DirectoryJson.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "common/chat/MessagingLimits.h"
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<DirectChatTarget> AuthorizeRequest(IThreadStore& store, const ChatBlobRequest& request,
                                       const std::string& local_relay_user_id, const ChatBlobRequester& requester) {
  if (local_relay_user_id.empty()) {
    return Error("Local relay identity missing");
  }
  if (request.peer_identity_value != local_relay_user_id) {
    return Error("Chat-blob request targets a different peer stream");
  }
  if (request.requester_identity_value.empty() || request.thread_id.empty()) {
    return Error("Chat-blob request missing auth fields");
  }
  // The requester is who signed the request, on the link it signed for — not who the body names.
  if (!requester.signing_keys) {
    return Error("Request not authorized");
  }
  if (auto authorized =
          PeerRequestAuth::Verify(request, requester.remote_peer_id, requester.now_ms, *requester.signing_keys);
      !authorized) {
    return authorized.error();
  }

  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  target.peer_identity_value = request.requester_account_id;
  target.channel = request.channel;

  auto thread = store.FindDirectThread(target);
  if (!thread || !*thread) {
    return Error("Requester is not a chat participant");
  }
  if ((*thread)->id != request.thread_id) {
    return Error("Chat-blob thread mismatch");
  }
  if (!ThreadChannelIsE2e((*thread)->channel)) {
    return Error("Chat-blob requires E2E direct thread");
  }
  return target;
}

Roe<ChatAttachmentFields> FindAttachmentFields(IThreadStore& store, const std::string& thread_id,
                                               const std::vector<uint8_t>& content_hash) {
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return page.error();
  }
  for (const ThreadMessage& message : *page) {
    // Only what this device sent in the thread: the requester is the one it was sent to. A row the
    // peer sent us names a key of the peer's choosing, so it never authorizes serving our content.
    if (message.content_type != ChatContentType::Attachment || message.sender_contact_id != kLocalSelfContactId) {
      continue;
    }
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
    if (!fields || fields->content_hash.size() != kAttachmentContentHashSize) {
      continue;
    }
    if (fields->content_hash == content_hash) {
      return *fields;
    }
  }
  return Error("Attachment not found in thread history");
}

/** Hex content hashes of every attachment message in the thread (pending blobs they name are kept). */
std::unordered_set<std::string> ReferencedAttachmentHashes(IThreadStore& store, const std::string& thread_id) {
  std::unordered_set<std::string> hashes;
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return hashes;
  }
  for (const ThreadMessage& message : *page) {
    if (message.content_type != ChatContentType::Attachment) {
      continue;
    }
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
    if (fields && fields->content_hash.size() == kAttachmentContentHashSize) {
      hashes.insert(AttachmentHashHex(fields->content_hash));
    }
  }
  return hashes;
}

} // namespace

Roe<std::vector<uint8_t>> ChatBlobResponder::ServeFetch(IThreadStore& store, const ChatBlobRequest& request,
                                                        const std::string& local_relay_user_id,
                                                        const std::string& profile_data_dir,
                                                        const ChatBlobContentSource& content,
                                                        const ChatBlobRequester& requester) {
  if (request.op == ChatBlobOp::FetchPublic) {
    if (local_relay_user_id.empty()) {
      return Error("Local relay identity missing");
    }
    if (request.peer_identity_value != local_relay_user_id) {
      return Error("Chat-blob request targets a different peer stream");
    }
    if (request.requester_identity_value.empty()) {
      return Error("Public CAS fetch missing requester identity");
    }
    if (!content.pinned_public) {
      return Error("Public content is not served here");
    }
    auto content_id = HexToBytes(request.content_hash_hex);
    if (!content_id || content_id->size() != kAttachmentContentHashSize) {
      return Error("Invalid public CAS content id");
    }
    auto bytes = content.pinned_public(*content_id);
    if (!bytes) {
      return bytes.error();
    }
    if (bytes->size() > kMaxDirectAttachmentBytes) {
      return Error("Public CAS object too large");
    }
    return std::vector<uint8_t>(bytes->begin(), bytes->end());
  }

  if (request.op != ChatBlobOp::Fetch) {
    return Error("Expected fetch chat-blob request");
  }
  auto auth = AuthorizeRequest(store, request, local_relay_user_id, requester);
  if (!auth) {
    return auth.error();
  }

  auto hash = HexToBytes(request.content_hash_hex);
  if (!hash || hash->size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash");
  }

  auto fields = FindAttachmentFields(store, request.thread_id, *hash);
  if (!fields) {
    return fields.error();
  }

  auto pending = LoadPendingAttachmentCiphertext(profile_data_dir, request.thread_id, *hash);
  if (pending) {
    return std::vector<uint8_t>(pending->begin(), pending->end());
  }

  if (!content.private_plaintext) {
    return Error("Attachment content unavailable (profile locked)");
  }
  auto plaintext = content.private_plaintext(fields->content_hash);
  if (!plaintext) {
    return plaintext.error();
  }

  auto encrypted = AttachmentContentCipher::EncryptWithNonce(fields->content_key, *plaintext, fields->blob_nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  return std::vector<uint8_t>(encrypted->ciphertext.begin(), encrypted->ciphertext.end());
}

Roe<void> ChatBlobResponder::ServePush(IThreadStore& store, const ChatBlobRequest& request,
                                       const std::string& local_relay_user_id, const std::string& profile_data_dir,
                                       const std::vector<uint8_t>& ciphertext, const ChatBlobRequester& requester,
                                  const ChatBlobPushGate& push_gate) {
  if (request.op != ChatBlobOp::Push) {
    return Error("Expected push chat-blob request");
  }
  auto auth = AuthorizeRequest(store, request, local_relay_user_id, requester);
  if (!auth) {
    return auth.error();
  }
  if (ciphertext.empty()) {
    return Error("Empty chat-blob push body");
  }
  if (ciphertext.size() > kMaxDirectAttachmentBytes + 64) {
    return Error("Chat-blob push body too large");
  }

  auto hash = HexToBytes(request.content_hash_hex);
  if (!hash || hash->size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash");
  }
  // A changed client can push as fast as it likes: the receiver's own pace and disk limits decide.
  if (push_gate && !push_gate(request.requester_identity_value, ciphertext.size())) {
    return Error("Chat-blob push over the receive limits");
  }

  // Senders push before the attachment message itself arrives, so the hash cannot be required
  // to already be in thread history. SavePendingAttachmentCiphertext bounds orphan pushes (count
  // + TTL) per thread instead; blobs the thread's messages already name are exempt (they may sit
  // unopened for longer when auto-download deferred them).
  return SavePendingAttachmentCiphertext(profile_data_dir, request.thread_id, *hash, ciphertext,
                                         ReferencedAttachmentHashes(store, request.thread_id));
}

} // namespace pbr
