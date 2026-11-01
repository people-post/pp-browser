#include "domain/attachments/AttachmentClient.h"

#include "domain/attachments/ChatBlobRequest.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/Logger.h"
#include "foundation/error/AppError.h"

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

/** One WARNING per send whose direct push failed: the error text and the size only (no peer, key or file name). */
void LogDirectPushFailed(const uint64_t plaintext_bytes, const Error& why) {
  logging::getLogger("AttachmentClient").warning
      << "attachment direct push failed, falling back to the server bytes=" << plaintext_bytes
      << " reason=" << why.message;
}

/** A server answer "uploads not enabled" after a failed direct push says so: the user is told about both. */
Error NoteDirectFailed(Error server_error) {
  if (AppError::CategoryOf(server_error) == ErrorCategory::Blob &&
      server_error.code == static_cast<int32_t>(Err::Blob::UploadNotConfigured)) {
    return AppError::Blob(Err::Blob::UploadNotConfiguredNoDirect, server_error.message);
  }
  return server_error;
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

/** Direct push of a staged attachment when allowed, else (or on failure) the server, streamed from the file. */
void DeliverStaged(IBlobClient& blob, IdentityStore& identity, OutgoingAttachmentRecord record,
                   std::shared_ptr<PreparedChatAttachment> in_memory, ChatAttachmentUploadOptions options,
                   std::function<void(Roe<ChatAttachmentFields>)> finish) {
  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    finish(relay_user_id.error());
    return;
  }
  if (record.delivered) {
    finish(record.fields);
    return;
  }
  const std::string relay_id = *relay_user_id;
  OutgoingAttachmentStore* store = options.outgoing;
  const ByteVector dek = options.dek;
  auto to_server = [&blob, relay_id, record, store, dek, progress = options.progress,
                    finish](const bool direct_failed) mutable {
    auto uploaded = UploadRelayBlobFile(blob, relay_id, "application/octet-stream", BlobPurpose::File,
                                        store->CipherPath(record.id), record.cipher_bytes, progress);
    if (!uploaded) {
      finish(direct_failed ? NoteDirectFailed(uploaded.error()) : uploaded.error());
      return;
    }
    record.fields.url = uploaded.value().public_url;
    record.delivered = true;
    (void)store->Update(record, dek);
    finish(record.fields);
  };

  if (AttachmentTriesPeerDirect(options.thread, record.fields.byte_length)) {
    if (!in_memory) {
      // Small enough for the direct path: read the staged file back (never done for a large one).
      if (auto cipher = store->ReadCipher(record.id)) {
        in_memory = std::make_shared<PreparedChatAttachment>();
        in_memory->fields = record.fields;
        in_memory->ciphertext = std::move(*cipher);
      }
    }
    if (in_memory) {
      TryPeerPushAsync(*in_memory, identity, options,
                       [record, store, dek, finish, to_server = std::move(to_server), in_memory](Roe<void> pushed) mutable {
                         auto next = [record, store, dek, finish, to_server = std::move(to_server),
                                      pushed = std::move(pushed)]() mutable {
                           if (pushed) {
                             record.delivered = true;
                             (void)store->Update(record, dek);
                             finish(record.fields);
                             return;
                           }
                           LogDirectPushFailed(record.fields.byte_length, pushed.error());
                           to_server(true);
                         };
                         if (AppRuntime::IsRunning()) {
                           AppRuntime::PostWorkerNormal(std::move(next));
                         } else {
                           next();
                         }
                       });
      return;
    }
  }
  to_server(false);
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

  if (options.outgoing) {
    OutgoingAttachmentRecord record;
    record.id = OutgoingAttachmentStore::NewId();
    record.thread_id = options.thread_id;
    record.fields = prepared->fields;
    record.created_ms = util::NowUnixMs();
    if (auto staged = options.outgoing->Begin(record, prepared->ciphertext, options.dek); !staged) {
      finish(staged.error());
      return;
    }
    record.cipher_bytes = prepared->ciphertext.size();
    if (options.on_staged) {
      options.on_staged(record);
    }
    // A small ciphertext stays in memory for the direct push; a large one is dropped (it is on disk now).
    std::shared_ptr<PreparedChatAttachment> in_memory;
    if (AttachmentTriesPeerDirect(options.thread, plaintext.size())) {
      in_memory = std::make_shared<PreparedChatAttachment>(std::move(*prepared));
    } else {
      std::vector<uint8_t>().swap(prepared->ciphertext);
    }
    DeliverStaged(blob, identity, std::move(record), std::move(in_memory), std::move(options), finish);
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
            LogDirectPushFailed(prepared_shared->fields.byte_length, pushed.error());
            auto uploaded = UploadPreparedToRelay(blob, relay_id, *prepared_shared);
            if (!uploaded) {
              finish(NoteDirectFailed(uploaded.error()));
              return;
            }
            finish(std::move(uploaded));
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

void ResumeChatAttachmentAsync(IBlobClient& blob, IdentityStore& identity, const OutgoingAttachmentRecord& record,
                               ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatAttachmentFields> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!options.outgoing) {
    finish(Error("No outgoing attachment store"));
    return;
  }
  if (!record.delivered && !options.outgoing->HasCipher(record.id)) {
    finish(Error("The encrypted copy of the attachment is gone"));
    return;
  }
  DeliverStaged(blob, identity, record, nullptr, std::move(options), finish);
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
