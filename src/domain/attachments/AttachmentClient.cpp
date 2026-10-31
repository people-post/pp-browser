#include "domain/attachments/AttachmentClient.h"

#include "domain/attachments/ChatBlobRequest.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<std::string> RequireRegisteredRelayUserId(IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->relay_user_id.empty()) {
    return Error("Register on the network before sending attachments");
  }
  return loaded->relay_user_id;
}

std::string FilenameFromPath(const std::string& path) {
  const std::filesystem::path file_path(path);
  if (file_path.has_filename()) {
    return file_path.filename().string();
  }
  return "attachment";
}

void TryPeerPushAsync(const PreparedChatAttachment& prepared, IdentityStore& identity,
                      const ChatAttachmentUploadOptions& options,
                      std::function<void(Roe<void>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<void> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!options.peer_client || !options.contacts || !options.thread || options.thread_id.empty()) {
    finish(Error("Peer blob client not configured"));
    return;
  }
  if (options.thread->kind != ThreadKind::Direct || !ThreadChannelIsE2e(options.thread->channel)) {
    finish(Error("Peer blob push requires E2E direct thread"));
    return;
  }
  auto request = BuildChatBlobRequest(*options.thread, *options.contacts, identity, ChatBlobOp::Push,
                                      options.thread_id, prepared.fields.content_hash);
  if (!request) {
    finish(request.error());
    return;
  }
  if (!options.peer_client->IsPeerReachable(request->peer_identity_value)) {
    finish(Error("Peer-direct endpoint not registered"));
    return;
  }
  options.peer_client->PushChatBlobAsync(*request, prepared.ciphertext, std::move(finish));
}

Roe<ChatAttachmentFields> UploadPreparedToRelay(IBlobClient& blob, const std::string& relay_user_id,
                                                const PreparedChatAttachment& prepared) {
  const std::string body(reinterpret_cast<const char*>(prepared.ciphertext.data()), prepared.ciphertext.size());
  auto uploaded =
      UploadRelayBlobBytes(blob, relay_user_id, "application/octet-stream", BlobPurpose::File, body);
  if (!uploaded) {
    return uploaded.error();
  }

  ChatAttachmentFields fields = prepared.fields;
  fields.url = uploaded.value().public_url;
  return fields;
}

} // namespace

bool AttachmentTriesPeerDirect(const std::optional<Thread>& thread, const uint64_t plaintext_bytes) {
  return thread && thread->kind == ThreadKind::Direct && ThreadChannelIsE2e(thread->channel) &&
         plaintext_bytes <= kMaxDirectAttachmentBytes;
}

void UploadChatAttachmentAsync(IBlobClient& blob, IdentityStore& identity, const std::vector<uint8_t>& plaintext,
                               const std::string& filename, const std::string& mime,
                               ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatAttachmentFields> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };

  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    finish(relay_user_id.error());
    return;
  }

  auto prepared = PrepareChatAttachmentFromBytes(plaintext, filename, mime, options.max_plaintext_bytes);
  if (!prepared) {
    finish(prepared.error());
    return;
  }

  if (AttachmentTriesPeerDirect(options.thread, plaintext.size())) {
    // Shared, not moved into the callback: the push reads the prepared bytes in the same call that builds
    // the callback (a moved-from copy there once made every direct push fail on "invalid content hash").
    const std::string relay_id = *relay_user_id;
    const auto prepared_shared = std::make_shared<PreparedChatAttachment>(std::move(*prepared));
    TryPeerPushAsync(
        *prepared_shared, identity, options,
        [&blob, relay_id, prepared_shared, finish](Roe<void> pushed) mutable {
          auto continue_result = [&blob, relay_id, prepared_shared, finish = std::move(finish),
                                  pushed = std::move(pushed)]() mutable {
            if (pushed) {
              finish(prepared_shared->fields);
              return;
            }
            finish(UploadPreparedToRelay(blob, relay_id, *prepared_shared));
          };
          if (AppRuntime::IsRunning()) {
            AppRuntime::PostWorkerNormal(std::move(continue_result));
          } else {
            continue_result();
          }
        });
    return;
  }

  finish(UploadPreparedToRelay(blob, *relay_user_id, *prepared));
}

void UploadChatAttachmentFromFileAsync(IBlobClient& blob, IdentityStore& identity, const std::string& path,
                                       ChatAttachmentUploadOptions options,
                                       std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec && size > options.max_plaintext_bytes) {
    if (on_done) {
      on_done(Error("Attachment is over the size limit")); // before the file is read
    }
    return;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (on_done) {
      on_done(Error("Could not read attachment file"));
    }
    return;
  }
  const std::vector<uint8_t> plaintext((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  UploadChatAttachmentAsync(blob, identity, plaintext, FilenameFromPath(path), std::string(), std::move(options),
                            std::move(on_done));
}

Roe<ChatAttachmentFields> UploadChatAttachmentFromFile(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& path,
                                                       const ChatAttachmentUploadOptions& options) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatAttachmentFields>>>();
  auto result_future = result_promise->get_future();
  UploadChatAttachmentFromFileAsync(blob, identity, path, options,
                                    [result_promise](Roe<ChatAttachmentFields> value) {
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
    return Error("Attachment upload timed out");
  }
  return result_future.get();
}

} // namespace pbr
