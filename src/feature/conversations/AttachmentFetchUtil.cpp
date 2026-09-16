#include "feature/conversations/AttachmentFetchUtil.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "domain/messaging/AttachmentCache.h"
#include "feature/conversations/ChatBlobRequestUtil.h"
#include "domain/net/HttpClient.h"
#include "common/PbrCompat.h"

#include <chrono>
#include <future>
#include <thread>

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

void FetchAttachmentCiphertextFromPeerAsync(const ChatAttachmentFields& fields,
                                            const AttachmentFetchContext& context,
                                            std::function<void(Roe<std::vector<uint8_t>>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<std::vector<uint8_t>> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!context.peer_client || !context.store || !context.contacts || !context.identity ||
      context.thread_id.empty()) {
    finish(Error("Peer blob client not configured"));
    return;
  }
  auto thread = context.store->GetThread(context.thread_id);
  if (!thread || !*thread) {
    finish(Error("Thread not found"));
    return;
  }
  auto request = BuildChatBlobRequest(**thread, *context.contacts, *context.identity, ChatBlobOp::Fetch,
                                      context.thread_id, fields.content_hash);
  if (!request) {
    finish(request.error());
    return;
  }
  if (!context.peer_client->IsPeerReachable(request->peer_identity_value)) {
    finish(Error("Peer-direct endpoint not registered"));
    return;
  }
  context.peer_client->FetchChatBlobAsync(*request, std::move(finish));
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

void FetchAttachmentCiphertextAsync(const ChatAttachmentFields& fields, const AttachmentFetchContext& context,
                                    std::function<void(Roe<std::vector<uint8_t>>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<std::vector<uint8_t>> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (fields.content_hash.size() == kAttachmentContentHashSize && !context.profile_data_dir.empty() &&
      !context.thread_id.empty()) {
    if (auto pending = LoadPendingAttachmentCiphertext(context.profile_data_dir, context.thread_id,
                                                       fields.content_hash);
        pending) {
      finish(std::vector<uint8_t>(pending->begin(), pending->end()));
      return;
    }
  }

  if (context.peer_client && context.store && context.contacts && context.identity && !context.thread_id.empty()) {
    FetchAttachmentCiphertextFromPeerAsync(fields, context, [fields, finish](Roe<std::vector<uint8_t>> peer_bytes) {
      if (peer_bytes) {
        finish(std::move(peer_bytes));
        return;
      }
      finish(FetchAttachmentCiphertextFromCdn(fields));
    });
    return;
  }

  finish(FetchAttachmentCiphertextFromCdn(fields));
}

Roe<std::vector<uint8_t>> FetchAttachmentCiphertext(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context) {
  auto result_promise = std::make_shared<std::promise<Roe<std::vector<uint8_t>>>>();
  auto result_future = result_promise->get_future();
  FetchAttachmentCiphertextAsync(fields, context, [result_promise](Roe<std::vector<uint8_t>> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(60000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Attachment fetch timed out");
  }
  return result_future.get();
}

void FetchAndDecryptAttachmentAsync(const ChatAttachmentFields& fields, const AttachmentFetchContext& context,
                                    std::function<void(Roe<std::vector<uint8_t>>)> on_done) {
  FetchAttachmentCiphertextAsync(fields, context, [fields, context, on_done = std::move(on_done)](
                                                      Roe<std::vector<uint8_t>> ciphertext) {
    if (!ciphertext) {
      if (on_done) {
        on_done(ciphertext.error());
      }
      return;
    }

    const ByteVector cipher_bytes(ciphertext->begin(), ciphertext->end());
    auto plaintext = AttachmentContentCipher::Decrypt(fields.content_key, fields.blob_nonce, cipher_bytes,
                                                      fields.content_hash);
    if (!plaintext) {
      if (on_done) {
        on_done(plaintext.error());
      }
      return;
    }

    if (!context.profile_data_dir.empty() && !context.thread_id.empty() &&
        fields.content_hash.size() == kAttachmentContentHashSize) {
      RemovePendingAttachmentCiphertext(context.profile_data_dir, context.thread_id, fields.content_hash);
    }

    if (on_done) {
      on_done(std::vector<uint8_t>(plaintext->begin(), plaintext->end()));
    }
  });
}

Roe<std::vector<uint8_t>> FetchAndDecryptAttachment(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context) {
  auto result_promise = std::make_shared<std::promise<Roe<std::vector<uint8_t>>>>();
  auto result_future = result_promise->get_future();
  FetchAndDecryptAttachmentAsync(fields, context, [result_promise](Roe<std::vector<uint8_t>> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(60000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Attachment fetch timed out");
  }
  return result_future.get();
}

} // namespace pbr
