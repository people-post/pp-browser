#include "base/messaging/ChatBlobResponder.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoUtil.h"
#include "base/messaging/AttachmentCache.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "common/chat/MessagingLimits.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<DirectChatTarget> AuthorizeRequest(IThreadStore& store, const ChatBlobRequest& request,
                                       const std::string& local_relay_user_id) {
  if (local_relay_user_id.empty()) {
    return Error("Local relay identity missing");
  }
  if (request.peer_identity_value != local_relay_user_id) {
    return Error("Chat-blob request targets a different peer stream");
  }
  if (request.requester_identity_value.empty() || request.thread_id.empty()) {
    return Error("Chat-blob request missing auth fields");
  }

  DirectChatTarget target;
  target.peer_identity_kind = request.peer_identity_kind;
  target.peer_identity_value = request.requester_identity_value;
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
    if (message.content_type != ChatContentType::Attachment) {
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

} // namespace

Roe<std::vector<uint8_t>> ChatBlobResponder::ServeFetch(IThreadStore& store, const ChatBlobRequest& request,
                                                        const std::string& local_relay_user_id,
                                                        const std::string& profile_data_dir, const ByteVector* dek,
                                                        std::string_view profile_id) {
  if (request.op != ChatBlobOp::Fetch) {
    return Error("Expected fetch chat-blob request");
  }
  auto auth = AuthorizeRequest(store, request, local_relay_user_id);
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

  auto plaintext = LoadAttachmentPlaintext(profile_data_dir, request.thread_id, fields->content_hash, fields->mime,
                                           fields->filename, dek, profile_id);
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
                                       const std::vector<uint8_t>& ciphertext) {
  if (request.op != ChatBlobOp::Push) {
    return Error("Expected push chat-blob request");
  }
  auto auth = AuthorizeRequest(store, request, local_relay_user_id);
  if (!auth) {
    return auth.error();
  }
  if (ciphertext.empty()) {
    return Error("Empty chat-blob push body");
  }
  if (ciphertext.size() > kMaxChatAttachmentPlaintextBytes + 64) {
    return Error("Chat-blob push body too large");
  }

  auto hash = HexToBytes(request.content_hash_hex);
  if (!hash || hash->size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment content hash");
  }

  return SavePendingAttachmentCiphertext(profile_data_dir, request.thread_id, *hash, ciphertext);
}

} // namespace pbr
