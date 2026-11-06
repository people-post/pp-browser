#include "domain/attachments/AttachmentClient.h"

#include "domain/attachments/ChatBlobRequest.h"
#include "domain/attachments/RelayFileUrl.h"
#include "lib/delivery/AttachmentContentCipher.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/Logger.h"
#include "foundation/error/AppError.h"
#include "foundation/runtime/BackgroundActivity.h"
#include "lib/net/BlobQuota.h"

#include <algorithm>
#include <cctype>
#include <memory>
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
  const BackgroundActivity background("attachment upload");
  const std::string body(reinterpret_cast<const char*>(prepared.ciphertext.data()), prepared.ciphertext.size());
  auto uploaded = UploadRelayFileBytes(blob, relay_user_id, body);
  if (!uploaded) {
    return uploaded.error();
  }

  ChatAttachmentFields fields = prepared.fields;
  fields.url = uploaded->url;
  return fields;
}

/**
 * The person's cancel (`options.cancelled`): the upload's progress answers "stop" from then on, and whatever result
 * comes after it becomes "cancelled": the staged copy (if any) is removed and a server copy deleted, so the caller
 * sends no message. True when it was cancelled already (`finish` has been called).
 */
bool HonourCancel(IBlobClient& blob, const std::string& relay_id, ChatAttachmentUploadOptions& options,
                  OutgoingAttachmentStore* store, const std::string& record_id,
                  std::function<void(Roe<ChatAttachmentFields>)>& finish) {
  const auto cancelled = options.cancelled;
  if (!cancelled) {
    return false;
  }
  options.progress = [cancelled, progress = std::move(options.progress)](const uint64_t done, const uint64_t total) {
    return !cancelled->load() && (!progress || progress(done, total));
  };
  finish = [&blob, relay_id, store, record_id, cancelled, finish = std::move(finish)](Roe<ChatAttachmentFields> result) {
    if (!cancelled->load()) {
      finish(std::move(result));
      return;
    }
    if (store != nullptr && !record_id.empty()) {
      store->Remove(record_id);
    }
    if (result) {
      (void)DeleteUploadedAttachmentCopy(blob, relay_id, result->url); // uploaded before the cancel landed
    }
    finish(Error("Attachment send cancelled"));
  };
  if (cancelled->load()) {
    finish(Error("Attachment send cancelled"));
    return true;
  }
  return false;
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
  const std::string relay_id = *relay_user_id;
  OutgoingAttachmentStore* store = options.outgoing;
  const ByteVector dek = options.dek;
  if (HonourCancel(blob, relay_id, options, store, record.id, finish)) {
    return;
  }
  if (record.delivered) {
    finish(record.fields);
    return;
  }
  auto to_server = [&blob, relay_id, record, store, dek, progress = options.progress,
                    finish](const bool direct_failed) mutable {
    // start → PUT → complete → stored link: all of it, also if the app goes to the background meanwhile.
    const BackgroundActivity background("attachment upload");
    auto uploaded = UploadRelayFile(blob, relay_id, store->CipherPath(record.id), record.cipher_bytes, progress);
    if (!uploaded) {
      finish(direct_failed ? NoteDirectFailed(uploaded.error()) : uploaded.error());
      return;
    }
    record.fields.url = uploaded->url;
    record.delivered = true;
    if (auto saved = store->Update(record, dek); !saved) {
      // The link is given once; a copy whose link is not kept cannot be sent later, so it goes and this send fails
      // (a retry uploads again).
      logging::getLogger("AttachmentClient").info << "relay file abandon blob=" << uploaded->blob_id
                                                  << " step=store code=none";
      (void)blob.FileDelete(relay_id, uploaded->blob_id);
      finish(saved.error());
      return;
    }
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

Roe<void> DeleteUploadedAttachmentCopy(IBlobClient& blob, const std::string& relay_user_id, const std::string& url) {
  if (url.empty()) {
    return {}; // sent peer-direct: nothing on the server
  }
  const std::string blob_id = RelayFileUrl::BlobIdOf(url);
  if (blob_id.empty()) {
    // An old CDN link: nothing this account can delete through the files relay.
    logging::getLogger("AttachmentClient").info << "uploaded copy skipped: not a relay file link";
    return {};
  }
  auto deleted = DeleteRelayFile(blob, relay_user_id, blob_id);
  if (!deleted && !(AppError::CategoryOf(deleted.error()) == ErrorCategory::Blob &&
                    deleted.error().code == static_cast<int32_t>(Err::Blob::Expired))) {
    Error failed = deleted.error(); // its own code (network, rate limit, ...), with the id for the log
    failed.message = "blob=" + blob_id + " " + failed.message;
    return failed;
  }
  return {}; // deleted, or already gone
}

uint64_t AttachmentQuotaNeededBytes(const Error& quota_error) {
  const uint64_t asked = RelayFileQuotaNeededBytes(quota_error);
  // No size in the error: free enough for the largest attachment this client sends.
  return asked > 0 ? asked : kDefaultAttachmentSendMaxBytes + kAttachmentCipherOverheadBytes;
}

bool AttachmentTriesPeerDirect(const std::optional<Thread>& thread, const uint64_t plaintext_bytes) {
  return thread && thread->kind == ThreadKind::Direct && ThreadChannelIsE2e(thread->channel) &&
         plaintext_bytes <= kMaxDirectAttachmentBytes;
}

void UploadChatAttachmentAsync(IBlobClient& blob, IdentityStore& identity, const std::vector<uint8_t>& plaintext,
                               const std::string& filename, const std::string& mime,
                               ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  std::function<void(Roe<ChatAttachmentFields>)> finish = [on_done = std::move(on_done)](
                                                               Roe<ChatAttachmentFields> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };

  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    finish(relay_user_id.error());
    return;
  }

  // Read before `on_staged`: the caller may free the plaintext there (it is not touched again after staging).
  const size_t plaintext_size = plaintext.size();
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
    if (AttachmentTriesPeerDirect(options.thread, plaintext_size)) {
      in_memory = std::make_shared<PreparedChatAttachment>(std::move(*prepared));
    } else {
      std::vector<uint8_t>().swap(prepared->ciphertext);
    }
    DeliverStaged(blob, identity, std::move(record), std::move(in_memory), std::move(options), finish);
    return;
  }

  // Not staged (no profile key yet): the cancel is honoured here too, between the steps (nothing is sent after it).
  if (HonourCancel(blob, *relay_user_id, options, nullptr, std::string(), finish)) {
    return;
  }
  if (AttachmentTriesPeerDirect(options.thread, plaintext_size)) {
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

} // namespace pbr
