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
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<std::string> RequireRegisteredAccountId(IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->account_id.empty()) {
    return Error("Register on the network before sending attachments");
  }
  return loaded->account_id;
}

void TryPeerPushAsync(const PreparedChatAttachment& prepared, IdentityStore& identity,
                      const ChatAttachmentUploadOptions& options, std::function<void(Roe<void>)> on_done) {
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
  auto request = BuildChatBlobRequest(*options.thread, *options.contacts, identity, ChatBlobOp::Push, options.thread_id,
                                      prepared.fields.content_hash);
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

int64_t MsSince(const std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

/**
 * One WARNING per send whose direct push failed: the error text, the size and how long the push took before it gave
 * up (-1: not measured) (no peer, key or file name).
 */
void LogDirectPushFailed(const uint64_t plaintext_bytes, const Error& why, const int64_t direct_ms = -1) {
  logging::getLogger("AttachmentClient").warning
      << "attachment direct push failed, falling back to the server bytes=" << plaintext_bytes
      << " direct_ms=" << direct_ms << " reason=" << why.message;
}

/** A server answer "uploads not enabled" after a failed direct push says so: the user is told about both. */
Error NoteDirectFailed(Error server_error) {
  if (AppError::CategoryOf(server_error) == ErrorCategory::Blob &&
      server_error.code == static_cast<int32_t>(Err::Blob::UploadNotConfigured)) {
    return AppError::Blob(Err::Blob::UploadNotConfiguredNoDirect, server_error.message);
  }
  return server_error;
}

Roe<ChatAttachmentFields> UploadPreparedToRelay(IBlobClient& blob, const std::string& account_id,
                                                const PreparedChatAttachment& prepared) {
  const BackgroundActivity background("attachment upload");
  const std::string body(reinterpret_cast<const char*>(prepared.ciphertext.data()), prepared.ciphertext.size());
  auto uploaded = UploadRelayFileBytes(blob, account_id, body);
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
 * Too late: a direct push that already reached the contact (no url) is not undone; its result passes through
 * unchanged (the caller tells the person it was already sent). `resuming`: the staged record is the Retry chip's, so
 * a cancel only stops this attempt; the record (and a server copy it may now point at) stays.
 */
bool HonourCancel(const std::shared_ptr<IBlobClient>& blob, const std::string& relay_id,
                  ChatAttachmentUploadOptions& options, OutgoingAttachmentStore* store, const std::string& record_id,
                  std::function<void(Roe<ChatAttachmentFields>)>& finish, const bool resuming = false) {
  const auto cancelled = options.cancelled;
  if (!cancelled) {
    return false;
  }
  options.progress = [cancelled, progress = std::move(options.progress)](const uint64_t done, const uint64_t total) {
    return !cancelled->load() && (!progress || progress(done, total));
  };
  finish = [blob, relay_id, store, record_id, cancelled, resuming,
            finish = std::move(finish)](Roe<ChatAttachmentFields> result) {
    if (!cancelled->load() || (result && result->url.empty())) {
      finish(std::move(result)); // not cancelled, or the contact already has it (too late to cancel)
      return;
    }
    if (!resuming && store != nullptr && !record_id.empty()) {
      store->Remove(record_id);
    }
    if (result && !resuming) {
      (void)DeleteUploadedAttachmentCopy(*blob, relay_id, result->url); // uploaded before the cancel landed
    }
    finish(Error("Attachment send cancelled"));
  };
  if (cancelled->load()) {
    finish(Error("Attachment send cancelled"));
    return true;
  }
  return false;
}

/**
 * The two legs of a staged small attachment: the direct push and, once it has run for kDirectHeadStart (or at once on
 * its failure), the server upload. The first to deliver settles the send (`finish` is called once) and is the only one
 * to write the staged record; the other's late result is dropped: a server copy uploaded after the push won is deleted,
 * a push that lands after the server won changes nothing (the caller makes the one message from the one result).
 */
struct DeliveryRace {
  std::mutex mu;
  bool settled = false;
  /** A direct push was sent and has not answered. */
  bool direct_pending = false;
  bool direct_failed = false;
  bool server_started = false;
  /** The server leg failed while the direct push was still out: the push decides, this is the error if it fails. */
  std::optional<Error> server_error;
  /** Read by the server upload's progress: once the push has won, the upload stops at its next step. */
  std::atomic<bool> direct_won{false};
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  /** -1: that leg had not answered (or did not run) when the send settled. */
  int64_t direct_ms = -1;
  int64_t relay_ms = -1;
};

/** One INFO line per settled send: which leg delivered it and how long each leg took (sizes and times only). */
void LogDelivered(const char* won, const uint64_t plaintext_bytes, const int64_t direct_ms, const int64_t relay_ms,
                  const int64_t total_ms) {
  logging::getLogger("AttachmentClient").info << "attachment delivered won=" << won << " bytes=" << plaintext_bytes
                                              << " direct_ms=" << direct_ms << " relay_ms=" << relay_ms
                                              << " total_ms=" << total_ms;
}

/**
 * Direct push of a staged attachment when allowed, the server beside it after the push's head start (or on its
 * failure), else the server alone, streamed from the file.
 */
void DeliverStaged(const std::shared_ptr<IBlobClient>& blob, IdentityStore& identity, OutgoingAttachmentRecord record,
                   std::shared_ptr<PreparedChatAttachment> in_memory, ChatAttachmentUploadOptions options,
                   std::function<void(Roe<ChatAttachmentFields>)> finish, const bool resuming = false) {
  auto account_id = RequireRegisteredAccountId(identity);
  if (!account_id) {
    finish(account_id.error());
    return;
  }
  const std::string relay_id = *account_id;
  OutgoingAttachmentStore* store = options.outgoing;
  const ByteVector dek = options.dek;
  if (record.delivered) {
    // Already with the contact (or on the server, its link kept): a cancel is too late, the message is made.
    finish(record.fields);
    return;
  }
  if (HonourCancel(blob, relay_id, options, store, record.id, finish, resuming)) {
    return;
  }
  auto race = std::make_shared<DeliveryRace>();
  std::function<void()> to_server = [blob, relay_id, record, store, dek, progress = options.progress, finish,
                                     race]() mutable {
    // start → PUT → complete → stored link: all of it, also if the app goes to the background meanwhile.
    const BackgroundActivity background("attachment upload");
    const auto relay_started = std::chrono::steady_clock::now();
    auto progress_unless_direct_won = [race, progress](const uint64_t done, const uint64_t total) {
      return !race->direct_won.load() && (!progress || progress(done, total));
    };
    auto uploaded =
        UploadRelayFile(*blob, relay_id, store->CipherPath(record.id), record.cipher_bytes, progress_unless_direct_won);
    const int64_t relay_ms = MsSince(relay_started);
    logging::getLogger("AttachmentClient").info << "attachment server upload bytes=" << record.cipher_bytes
                                                << " relay_ms=" << relay_ms << " ok=" << (uploaded ? 1 : 0);
    std::unique_lock lock(race->mu);
    race->relay_ms = relay_ms;
    if (race->settled) {
      lock.unlock();
      // The direct push delivered first: no message will point at this copy.
      if (uploaded) {
        logging::getLogger("AttachmentClient").info << "attachment late server copy deleted, direct push won blob="
                                                    << uploaded->blob_id;
        if (auto deleted = blob->FileDelete(relay_id, uploaded->blob_id); !deleted) {
          // Left for the relay's own expiry; the send stands (the push delivered it).
          logging::getLogger("AttachmentClient").warning
              << "attachment late server copy not deleted blob=" << uploaded->blob_id
              << " reason=" << deleted.error().message;
        }
      }
      return;
    }
    if (!uploaded) {
      if (race->direct_pending) {
        race->server_error = uploaded.error(); // the push still out decides
        return;
      }
      race->settled = true;
      const bool direct_failed = race->direct_failed;
      lock.unlock();
      finish(direct_failed ? NoteDirectFailed(uploaded.error()) : uploaded.error());
      return;
    }
    race->settled = true;
    const int64_t direct_ms = race->direct_ms;
    lock.unlock();
    LogDelivered("server", record.fields.byte_length, direct_ms, relay_ms, MsSince(race->started));
    record.fields.url = uploaded->url;
    record.delivered = true;
    if (auto saved = store->Update(record, dek); !saved) {
      // The link is given once; a copy whose link is not kept cannot be sent later, so it goes and this send fails
      // (a retry uploads again).
      logging::getLogger("AttachmentClient").info << "relay file abandon blob=" << uploaded->blob_id
                                                  << " step=store code=none";
      (void)blob->FileDelete(relay_id, uploaded->blob_id);
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
      race->direct_pending = true; // nothing else holds `race` yet
      TryPeerPushAsync(
          *in_memory, identity, options,
          [record, store, dek, finish, to_server, in_memory, race](Roe<void> pushed) mutable {
            const int64_t direct_ms = MsSince(race->started);
            auto next = [record, store, dek, finish, to_server, race, pushed = std::move(pushed), direct_ms]() mutable {
              std::unique_lock lock(race->mu);
              race->direct_pending = false;
              race->direct_ms = direct_ms;
              if (race->settled) {
                lock.unlock();
                // The server delivered first: the message carries its link; this answer changes nothing.
                logging::getLogger("AttachmentClient").info
                    << "attachment late direct push ignored, server won ok=" << (pushed ? 1 : 0)
                    << " direct_ms=" << direct_ms;
                return;
              }
              if (pushed) {
                race->settled = true;
                race->direct_won.store(true);
                const int64_t relay_ms = race->relay_ms;
                lock.unlock();
                logging::getLogger("AttachmentClient").info
                    << "attachment direct push ok bytes=" << record.fields.byte_length << " direct_ms=" << direct_ms;
                LogDelivered("direct", record.fields.byte_length, direct_ms, relay_ms, direct_ms);
                record.delivered = true;
                (void)store->Update(record, dek);
                finish(record.fields);
                return;
              }
              race->direct_failed = true;
              if (!race->server_started) {
                race->server_started = true;
                lock.unlock();
                LogDirectPushFailed(record.fields.byte_length, pushed.error(), direct_ms);
                to_server();
                return;
              }
              if (race->server_error) {
                race->settled = true;
                const Error server_error = *race->server_error;
                lock.unlock();
                LogDirectPushFailed(record.fields.byte_length, pushed.error(), direct_ms);
                finish(NoteDirectFailed(server_error));
                return;
              }
              lock.unlock();
              LogDirectPushFailed(record.fields.byte_length, pushed.error(), direct_ms); // the upload under way decides
            };
            if (AppRuntime::IsRunning()) {
              AppRuntime::PostWorkerNormal(std::move(next));
            } else {
              next();
            }
          });
      // The head start: a push that has not answered by then gets the server upload beside it.
      auto start_beside = [race, to_server, cancelled = options.cancelled]() mutable {
        std::unique_lock lock(race->mu);
        if (race->settled || race->server_started || !race->direct_pending || (cancelled && cancelled->load())) {
          return; // answered already (its failure started the server), or cancelled: the push's answer decides
        }
        race->server_started = true;
        lock.unlock();
        logging::getLogger("AttachmentClient").info << "attachment direct push still out after "
                                                    << kDirectHeadStart.count()
                                                    << " ms, server upload starts beside it";
        to_server();
      };
      std::unique_lock lock(race->mu);
      if (!race->direct_pending) {
        return; // the push answered at once (not reachable, not addressable): no head start to give
      }
      lock.unlock();
      if (options.run_after) {
        options.run_after(kDirectHeadStart, std::move(start_beside));
      } else if (AppRuntime::IsRunning()) {
        AppRuntime::ScheduleCoordinatorOneShot(kDirectHeadStart, [start_beside]() {
          AppRuntime::PostWorkerNormal(start_beside); // the upload blocks: never on the coordinator
        });
      }
      return;
    }
  }
  race->server_started = true;
  to_server();
}

} // namespace

Roe<void> DeleteUploadedAttachmentCopy(IBlobClient& blob, const std::string& account_id, const std::string& url) {
  if (url.empty()) {
    return {}; // sent peer-direct: nothing on the server
  }
  const std::string blob_id = RelayFileUrl::BlobIdOf(url);
  if (blob_id.empty()) {
    // An old CDN link: nothing this account can delete through the files relay.
    logging::getLogger("AttachmentClient").info << "uploaded copy skipped: not a relay file link";
    return {};
  }
  auto deleted = DeleteRelayFile(blob, account_id, blob_id);
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

void UploadChatAttachmentAsync(std::shared_ptr<IBlobClient> blob, IdentityStore& identity,
                               const std::vector<uint8_t>& plaintext, const std::string& filename,
                               const std::string& mime, ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  std::function<void(Roe<ChatAttachmentFields>)> finish = [on_done =
                                                               std::move(on_done)](Roe<ChatAttachmentFields> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!blob) {
    finish(Error("Blob client not configured"));
    return;
  }
  auto account_id = RequireRegisteredAccountId(identity);
  if (!account_id) {
    finish(account_id.error());
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
  if (HonourCancel(blob, *account_id, options, nullptr, std::string(), finish)) {
    return;
  }
  if (AttachmentTriesPeerDirect(options.thread, plaintext_size)) {
    // Shared, not moved into the callback: the push reads the prepared bytes in the same call that builds
    // the callback (a moved-from copy there once made every direct push fail on "invalid content hash").
    const std::string relay_id = *account_id;
    const auto prepared_shared = std::make_shared<PreparedChatAttachment>(std::move(*prepared));
    TryPeerPushAsync(*prepared_shared, identity, options,
                     [blob, relay_id, prepared_shared, finish](Roe<void> pushed) mutable {
                       auto continue_result = [blob, relay_id, prepared_shared, finish = std::move(finish),
                                               pushed = std::move(pushed)]() mutable {
                         if (pushed) {
                           finish(prepared_shared->fields);
                           return;
                         }
                         LogDirectPushFailed(prepared_shared->fields.byte_length, pushed.error());
                         auto uploaded = UploadPreparedToRelay(*blob, relay_id, *prepared_shared);
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

  finish(UploadPreparedToRelay(*blob, *account_id, *prepared));
}

void ResumeChatAttachmentAsync(std::shared_ptr<IBlobClient> blob, IdentityStore& identity,
                               const OutgoingAttachmentRecord& record, ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatAttachmentFields> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!blob) {
    finish(Error("Blob client not configured"));
    return;
  }
  if (!options.outgoing) {
    finish(Error("No outgoing attachment store"));
    return;
  }
  if (!record.delivered && !options.outgoing->HasCipher(record.id)) {
    finish(Error("The encrypted copy of the attachment is gone"));
    return;
  }
  DeliverStaged(blob, identity, record, nullptr, std::move(options), finish, /*resuming=*/true);
}

} // namespace pbr
