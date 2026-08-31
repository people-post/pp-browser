#include "base/net/AttachmentFetchUtil.h"

#include "base/crypto/AttachmentContentCipher.h"
#include "base/crypto/AttachmentContentHash.h"
#include "base/messaging/AttachmentCache.h"
#include "base/messaging/ChatBlobRequestUtil.h"
#include "base/net/HttpClient.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<std::vector<uint8_t>> FetchAttachmentCiphertextFromCdn(const ChatAttachmentFields& fields) {
  if (fields.url.empty()) {
    return Error("Attachment URL is required");
  }
  const auto response = HttpClient::Get(fields.url);
  if (!response) {
    return response.error();
  }
  const HttpResponse& http = response.value();
  if (http.status_code < 200 || http.status_code >= 300) {
    return Error("Attachment download failed with status " + std::to_string(http.status_code));
  }
  if (http.body.empty()) {
    return Error("Attachment download returned empty body");
  }
  return std::vector<uint8_t>(http.body.begin(), http.body.end());
}

Roe<std::vector<uint8_t>> FetchAttachmentCiphertextFromPeer(const ChatAttachmentFields& fields,
                                                              const AttachmentFetchContext& context) {
  if (!context.peer_client || !context.store || !context.contacts || !context.identity ||
      context.thread_id.empty()) {
    return Error("Peer blob client not configured");
  }
  auto thread = context.store->GetThread(context.thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }
  auto request = BuildChatBlobRequest(**thread, *context.contacts, *context.identity, ChatBlobOp::Fetch,
                                      context.thread_id, fields.content_hash);
  if (!request) {
    return request.error();
  }
  if (!context.peer_client->IsPeerReachable(request->peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }
  return context.peer_client->FetchChatBlob(*request);
}

} // namespace

bool CanFetchAttachment(const ChatAttachmentFields& fields, const AttachmentFetchContext& context) {
  if (!fields.url.empty()) {
    return true;
  }
  if (!context.profile_data_dir.empty() && !context.thread_id.empty() &&
      fields.content_hash.size() == kAttachmentContentHashSize &&
      AttachmentPendingCiphertextExists(context.profile_data_dir, context.thread_id, fields.content_hash)) {
    return true;
  }
  if (context.peer_client && context.store && !context.thread_id.empty()) {
    return true;
  }
  return false;
}


Roe<std::vector<uint8_t>> FetchAttachmentCiphertext(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context) {
  if (fields.content_hash.size() == kAttachmentContentHashSize && !context.profile_data_dir.empty() &&
      !context.thread_id.empty()) {
    if (auto pending = LoadPendingAttachmentCiphertext(context.profile_data_dir, context.thread_id,
                                                       fields.content_hash);
        pending) {
      return std::vector<uint8_t>(pending->begin(), pending->end());
    }
  }

  if (context.peer_client && context.store && context.contacts && context.identity && !context.thread_id.empty()) {
    auto peer_bytes = FetchAttachmentCiphertextFromPeer(fields, context);
    if (peer_bytes) {
      return peer_bytes;
    }
  }

  return FetchAttachmentCiphertextFromCdn(fields);
}

Roe<std::vector<uint8_t>> FetchAndDecryptAttachment(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context) {
  auto ciphertext = FetchAttachmentCiphertext(fields, context);
  if (!ciphertext) {
    return ciphertext.error();
  }

  const ByteVector cipher_bytes(ciphertext->begin(), ciphertext->end());
  auto plaintext = AttachmentContentCipher::Decrypt(fields.content_key, fields.blob_nonce, cipher_bytes,
                                                    fields.content_hash);
  if (!plaintext) {
    return plaintext.error();
  }

  if (!context.profile_data_dir.empty() && !context.thread_id.empty() &&
      fields.content_hash.size() == kAttachmentContentHashSize) {
    RemovePendingAttachmentCiphertext(context.profile_data_dir, context.thread_id, fields.content_hash);
  }

  return std::vector<uint8_t>(plaintext->begin(), plaintext->end());
}

} // namespace pbr
