#include "domain/attachments/AttachmentFetch.h"

#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/AttachmentContentCipher.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/error/AppError.h"
#include "lib/messaging/AttachmentCache.h"
#include "domain/attachments/ChatBlobRequest.h"
#include "lib/net/HttpClient.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

namespace pbr {

namespace {

/** The ciphertext length a declared plaintext length implies; the declared length is the sender's word. */
uint64_t ExpectedCiphertextBytes(const ChatAttachmentFields& fields) {
  return fields.byte_length + kAttachmentCipherOverheadBytes;
}

Roe<std::vector<uint8_t>> FetchAttachmentCiphertextFromCdn(const ChatAttachmentFields& fields,
                                                           const AttachmentFetchContext& context) {
  if (fields.url.empty()) {
    return Error("Attachment URL is required");
  }
  if (fields.byte_length == 0) {
    return Error("Attachment declares no size");
  }
  const uint64_t cap = ExpectedCiphertextBytes(fields);
  // fields.url is peer-controlled (sender's attachment URL): restrict to https + public hosts, and stop at the
  // declared size (plus the AEAD tag) instead of taking whatever the host sends.
  if (context.profile_data_dir.empty()) {
    if (cap > kMaxHttpClientBodyBytes) {
      return Error("Attachment download needs a profile directory");
    }
    const auto response = HttpClient::Get(fields.url, {}, static_cast<size_t>(cap), HttpTimeout{},
                                          /*restrict_to_public_https=*/!context.allow_local_cdn_for_test);
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

  // Streamed to a temp file (no whole-body buffer in the HTTP layer), then read back once.
  const std::string dir = IncomingCiphertextDir(context.profile_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string part = (std::filesystem::path(dir) / (AttachmentHashHex(fields.content_hash) + ".part")).string();
  auto downloaded = HttpClient::GetToFile(fields.url, part, cap, context.progress,
                                          /*restrict_to_public_https=*/!context.allow_local_cdn_for_test);
  if (!downloaded) {
    return downloaded.error();
  }
  if (downloaded->status_code < 200 || downloaded->status_code >= 300) {
    return AppError::Blob(downloaded->status_code == 404 ? Err::Blob::Expired : Err::Blob::Generic,
                          "Attachment download failed with status " + std::to_string(downloaded->status_code));
  }
  std::ifstream input(part, std::ios::binary);
  std::vector<uint8_t> bytes(static_cast<size_t>(downloaded->bytes));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  const bool read_ok = static_cast<bool>(input) || input.gcount() == static_cast<std::streamsize>(bytes.size());
  input.close();
  std::filesystem::remove(part, ec);
  if (!read_ok || bytes.empty()) {
    return Error("Attachment download returned empty body");
  }
  return bytes;
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

std::string IncomingCiphertextDir(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "blob_cipher" / "incoming").string();
}

void CleanIncomingCiphertext(const std::string& profile_data_dir) {
  if (profile_data_dir.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(IncomingCiphertextDir(profile_data_dir), ec);
}

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

  // A large file with a server copy is fetched from the server: the direct path has a short timeout and the
  // sender's device is not asked to stream it.
  const bool large_with_url = fields.byte_length > kMaxDirectAttachmentBytes && !fields.url.empty();
  if (!large_with_url && context.peer_client && context.store && context.contacts && context.identity &&
      !context.thread_id.empty()) {
    FetchAttachmentCiphertextFromPeerAsync(fields, context, [fields, context, finish](Roe<std::vector<uint8_t>> peer_bytes) {
      if (peer_bytes) {
        finish(std::move(peer_bytes));
        return;
      }
      finish(FetchAttachmentCiphertextFromCdn(fields, context));
    });
    return;
  }

  finish(FetchAttachmentCiphertextFromCdn(fields, context));
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

    // The length must be exactly what the message declared, whichever way the bytes came (pending push, peer,
    // CDN): a different one is a lying sender or a corrupt object, never decrypted, and a pending copy is dropped.
    if (fields.byte_length == 0 || ciphertext->size() != ExpectedCiphertextBytes(fields)) {
      if (!context.profile_data_dir.empty() && !context.thread_id.empty() &&
          fields.content_hash.size() == kAttachmentContentHashSize) {
        RemovePendingAttachmentCiphertext(context.profile_data_dir, context.thread_id, fields.content_hash);
      }
      if (on_done) {
        on_done(Error("Attachment size does not match what the message declared"));
      }
      return;
    }

    // The ciphertext moves into the decrypt (a bad pending copy is dropped by hash below, not by its bytes), so
    // a large file is never held three times over.
    auto plaintext = AttachmentContentCipher::Decrypt(fields.content_key, fields.blob_nonce, std::move(*ciphertext),
                                                      fields.content_hash);
    if (!plaintext) {
      // Whatever we just fed to Decrypt was bad — most likely a poisoned/corrupt pending push
      // (see ChatBlobResponder::ServePush). Drop it so the next attempt re-fetches instead of
      // repeating the same failing decrypt forever.
      if (!context.profile_data_dir.empty() && !context.thread_id.empty() &&
          fields.content_hash.size() == kAttachmentContentHashSize) {
        RemovePendingAttachmentCiphertext(context.profile_data_dir, context.thread_id, fields.content_hash);
      }
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
      on_done(std::move(*plaintext));
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
