#include "feature/conversations/ChatSyncWorkflow.h"

#include "feature/conversations/InboxController.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "lib/messaging/ChatSyncLogic.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/messaging/RelayWirePayload.h"
#include "common/chat/MessagingLimits.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/PeerBriefRoute.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include "foundation/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

ChatSyncWorkflow::ChatSyncWorkflow(IThreadStore& store, IdentityStore& identity, ContactsStore& contacts,
                                 IRelayClient* relay, RelayReceivePipeline& receive_pipeline, InboxController& inbox,
                                 IChatHistoryPeerClient* peer_client)
    : store_(store), identity_(identity), contacts_(contacts), relay_(relay), peer_client_(peer_client),
      receive_pipeline_(receive_pipeline), inbox_(inbox) {}

void ChatSyncWorkflow::SetOnMessagesChanged(std::function<void()> callback) {
  on_messages_changed_ = std::move(callback);
}

void ChatSyncWorkflow::SetAttachmentDownloads(AttachmentFetchWorkflow* downloads) {
  attachment_downloads_ = downloads;
}

void ChatSyncWorkflow::SetPeerHistoryClient(IChatHistoryPeerClient* peer_client) {
  peer_client_ = peer_client;
}

Roe<ChatHistoryRequest> ChatSyncWorkflow::BuildRequest(const Thread& thread, const uint32_t session_epoch,
                                                      const uint64_t history_floor_seq,
                                                      const std::optional<uint64_t> min_sender_seq,
                                                      const std::optional<uint64_t> max_sender_seq, const size_t limit,
                                                      const std::string& order) const {
  auto local_identity = identity_.Get();
  if (!local_identity) {
    return local_identity.error();
  }
  if (thread.peer_identity_kind.empty() || thread.peer_identity_value.empty()) {
    return Error("Direct thread missing peer identity");
  }

  ChatHistoryRequest request;
  request.requester_identity_kind = ContactIdKindToString(mesh::ContactIdKind::RelayUser);
  request.requester_identity_value = local_identity->relay_user_id;
  // History/stream routing stays on Brief relay ids (M010); thread peer is Account ID.
  const std::string peer_relay = ContactRelayRoute(thread, contacts_).value_or(std::string());
  if (peer_relay.empty()) {
    return Error("Direct thread missing peer relay route for history");
  }
  request.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::RelayUser);
  request.peer_identity_value = peer_relay;
  request.channel = thread.channel;
  request.session_epoch = session_epoch;
  request.order = order;
  chat_sync::BoundHistoryRequest(request, history_floor_seq, min_sender_seq, max_sender_seq, limit);
  return request;
}

bool ChatSyncWorkflow::PassesEmptyGapGuard(const std::string& thread_id, const uint32_t session_epoch,
                                          const std::string& seq_owner, const uint64_t gap_max) const {
  SeqRangeQuery query;
  query.session_epoch = session_epoch;
  query.seq_owner_contact_id = seq_owner;
  query.min_sender_seq = gap_max + 1;
  query.limit = 1;
  auto rows = store_.GetMessagesBySeqRange(thread_id, query);
  if (!rows) {
    return false;
  }
  return rows->empty();
}

Roe<ChatSyncResult> ChatSyncWorkflow::IngestHistoryResponse(const std::string& thread_id,
                                                           const ChatHistoryRequest& request,
                                                           const ChatHistoryResponse& response,
                                                           const MessageTransport transport) {
  if (response.session_epoch != request.session_epoch) {
    return Error("History response epoch mismatch");
  }

  ChatSyncResult result;
  bool changed = false;

  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, request.session_epoch);
  if (!sync_state) {
    return sync_state.error();
  }
  const bool authorized_older_backfill = chat_sync::AuthorizedOlderBackfill(request, *sync_state);

  auto identity = identity_.Get();
  const std::string local_account_id = identity ? (*identity).account_id : std::string{};

  // No more than was asked for (and never past the batch cap), whatever the responder sent.
  const size_t cap = chat_sync::HistoryResponseCap(request);
  size_t taken = 0;
  for (const RelayEnvelope& envelope : response.messages) {
    if (taken++ >= cap) {
      break;
    }
    const RelayReceiveOutcome outcome =
        receive_pipeline_.ProcessEnvelope(envelope, local_account_id, authorized_older_backfill, transport);
    if (outcome.persisted) {
      ++result.ingested;
      changed = true;
      if (!outcome.thread_id.empty() && !outcome.deleted_by_sender) {
        inbox_.OnInboundMessagePersisted(outcome.thread_id);
        // The stored, decrypted message — never the wire payload (ciphertext).
        if (attachment_downloads_ && outcome.persisted_message &&
            outcome.persisted_message->content_type == ChatContentType::Attachment) {
          attachment_downloads_->EnqueueFromMessage(outcome.thread_id, *outcome.persisted_message);
        }
      }
    }
  }

  if (result.ingested == 0 && request.min_sender_seq && request.max_sender_seq) {
    auto fresh_sync_state = store_.GetPeerSyncState(thread_id, request.session_epoch);
    if (!fresh_sync_state) {
      return fresh_sync_state.error();
    }
    if (PassesEmptyGapGuard(thread_id, request.session_epoch, chat_sync::SeqOwner(**thread), *request.max_sender_seq)) {
      PeerSyncState updated = *fresh_sync_state;
      chat_sync::CloseEmptyGap(updated, *request.min_sender_seq, *request.max_sender_seq);
      (void)store_.SetPeerSyncState(thread_id, request.session_epoch, updated);
      result.empty_gap_closed = true;
    }
  }

  if (result.ingested > 0 || result.empty_gap_closed) {
    AdvanceContiguousThroughStoredSeqs(thread_id, request.session_epoch);
  }

  if (changed && on_messages_changed_) {
    on_messages_changed_();
  }

  return result;
}

void ChatSyncWorkflow::AdvanceContiguousThroughStoredSeqs(const std::string& thread_id,
                                                         const uint32_t session_epoch) {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, session_epoch);
  if (!sync_state) {
    return;
  }

  const std::string seq_owner = chat_sync::SeqOwner(**thread);
  PeerSyncState updated = *sync_state;
  const bool changed = chat_sync::AdvanceContiguous(updated, [&](const uint64_t seq) {
    SeqRangeQuery query;
    query.session_epoch = session_epoch;
    query.seq_owner_contact_id = seq_owner;
    query.min_sender_seq = seq;
    query.max_sender_seq = seq;
    query.limit = 1;
    auto rows = store_.GetMessagesBySeqRange(thread_id, query);
    return rows && !rows->empty();
  });
  if (changed) {
    (void)store_.SetPeerSyncState(thread_id, session_epoch, updated);
  }
}

Roe<ChatSyncResult> ChatSyncWorkflow::FetchChatTargetMessages(const std::string& thread_id,
                                                             ChatHistoryRequest request) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatSyncResult>>>();
  auto result_future = result_promise->get_future();
  FetchChatTargetMessagesAsync(thread_id, std::move(request), [result_promise](Roe<ChatSyncResult> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  // Tests / sync callers: peer path may complete on Amp callbacks; park briefly.
  constexpr auto kWait = std::chrono::milliseconds(12000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Chat history sync timed out");
  }
  return result_future.get();
}

void ChatSyncWorkflow::FetchChatTargetMessagesAsync(const std::string& thread_id, ChatHistoryRequest request,
                                                    std::function<void(Roe<ChatSyncResult>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatSyncResult> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };

  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    finish(Error("Thread not found"));
    return;
  }
  if ((*thread)->kind != ThreadKind::Direct || !ThreadChannelIsE2e((*thread)->channel)) {
    finish(Error("Sync requires E2E direct thread"));
    return;
  }

  auto sync_state = store_.GetPeerSyncState(thread_id, request.session_epoch);
  if (!sync_state) {
    finish(sync_state.error());
    return;
  }
  if (sync_state->phase == PeerSyncPhase::Compromised) {
    finish(Error("Sync disabled while thread is compromised"));
    return;
  }

  if (request.limit == 0) {
    request.limit = kDefaultTailSyncLimit;
  }
  request.limit = std::min(request.limit, kMaxPollBatchMessages);

  auto ingest_and_finish = [this, thread_id, request, finish](Roe<ChatHistoryResponse> response,
                                                              MessageTransport transport) {
    if (!response) {
      finish(response.error());
      return;
    }
    finish(IngestHistoryResponse(thread_id, request, *response, transport));
  };

  if (peer_client_ && peer_client_->IsPeerReachable(request.peer_identity_value)) {
    peer_client_->FetchChatHistoryAsync(
        request, [this, thread_id, request, finish, ingest_and_finish](Roe<ChatHistoryResponse> peer_response) {
          auto continue_on_worker = [this, thread_id, request, finish, ingest_and_finish,
                                     peer_response = std::move(peer_response)]() mutable {
            if (peer_response) {
              ingest_and_finish(std::move(peer_response), MessageTransport::Direct);
              return;
            }
            if (!relay_) {
              finish(Error("Relay client not configured"));
              return;
            }
            auto response = relay_->FetchChatHistory(request);
            if (!response) {
              finish(response.error());
              return;
            }
            ingest_and_finish(std::move(response), MessageTransport::Relay);
          };
          // Amp completions may arrive off-worker; tests often have no AppRuntime pool.
          if (AppRuntime::IsRunning()) {
            AppRuntime::PostWorkerNormal(std::move(continue_on_worker));
          } else {
            continue_on_worker();
          }
        });
    return;
  }

  if (!relay_) {
    finish(Error("Relay client not configured"));
    return;
  }
  auto response = relay_->FetchChatHistory(request);
  if (!response) {
    finish(response.error());
    return;
  }
  ingest_and_finish(std::move(response), MessageTransport::Relay);
}

Roe<ChatSyncResult> ChatSyncWorkflow::TailSync(const std::string& thread_id) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatSyncResult>>>();
  auto result_future = result_promise->get_future();
  TailSyncAsync(thread_id, [result_promise](Roe<ChatSyncResult> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(12000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Chat history sync timed out");
  }
  return result_future.get();
}

void ChatSyncWorkflow::TailSyncAsync(const std::string& thread_id,
                                     std::function<void(Roe<ChatSyncResult>)> on_done) {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    if (on_done) {
      on_done(Error("Thread not found"));
    }
    return;
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    if (on_done) {
      on_done(session_epoch.error());
    }
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    if (on_done) {
      on_done(sync_state.error());
    }
    return;
  }
  if (sync_state->phase == PeerSyncPhase::Compromised) {
    if (on_done) {
      on_done(Error("Sync disabled while thread is compromised"));
    }
    return;
  }

  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq,
                              chat_sync::TailMinSeq(*sync_state), std::nullopt,
                              kDefaultTailSyncLimit, "desc");
  if (!request) {
    if (on_done) {
      on_done(request.error());
    }
    return;
  }
  FetchChatTargetMessagesAsync(thread_id, *request, std::move(on_done));
}

Roe<ChatSyncResult> ChatSyncWorkflow::RepairGap(const std::string& thread_id, const uint64_t gap_min,
                                               const uint64_t gap_max) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatSyncResult>>>();
  auto result_future = result_promise->get_future();
  RepairGapAsync(thread_id, gap_min, gap_max, [result_promise](Roe<ChatSyncResult> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(12000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Chat history sync timed out");
  }
  return result_future.get();
}

void ChatSyncWorkflow::RepairGapAsync(const std::string& thread_id, const uint64_t gap_min, const uint64_t gap_max,
                                      std::function<void(Roe<ChatSyncResult>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatSyncResult> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    finish(Error("Thread not found"));
    return;
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    finish(session_epoch.error());
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    finish(sync_state.error());
    return;
  }
  if (sync_state->phase == PeerSyncPhase::Compromised) {
    finish(Error("Sync disabled while thread is compromised"));
    return;
  }

  const auto range = chat_sync::ClampGapRange(gap_min, gap_max, sync_state->history_floor_seq);
  if (!range) {
    finish(ChatSyncResult{});
    return;
  }

  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq, range->min, range->max,
                              kMaxPollBatchMessages, "asc");
  if (!request) {
    finish(request.error());
    return;
  }
  FetchChatTargetMessagesAsync(thread_id, *request, std::move(finish));
}

void ChatSyncWorkflow::MergeSyncResult(ChatSyncResult& aggregate, const ChatSyncResult& partial) const {
  aggregate.ingested += partial.ingested;
  aggregate.empty_gap_closed = aggregate.empty_gap_closed || partial.empty_gap_closed;
}

Roe<ChatSyncResult> ChatSyncWorkflow::RepairKnownGap(const std::string& thread_id) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatSyncResult>>>();
  auto result_future = result_promise->get_future();
  RepairKnownGapAsync(thread_id, [result_promise](Roe<ChatSyncResult> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(12000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Chat history sync timed out");
  }
  return result_future.get();
}

void ChatSyncWorkflow::RepairKnownGapAsync(const std::string& thread_id,
                                           std::function<void(Roe<ChatSyncResult>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatSyncResult> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    finish(session_epoch.error());
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    finish(sync_state.error());
    return;
  }
  const auto gap = chat_sync::KnownGap(*sync_state);
  if (!gap) {
    finish(ChatSyncResult{});
    return;
  }
  RepairGapAsync(thread_id, gap->min, gap->max, std::move(finish));
}

Roe<ChatSyncResult> ChatSyncWorkflow::ScrollBackfill(const std::string& thread_id) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatSyncResult>>>();
  auto result_future = result_promise->get_future();
  ScrollBackfillAsync(thread_id, [result_promise](Roe<ChatSyncResult> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(12000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Chat history sync timed out");
  }
  return result_future.get();
}

void ChatSyncWorkflow::ScrollBackfillAsync(const std::string& thread_id,
                                           std::function<void(Roe<ChatSyncResult>)> on_done) {
  auto finish = [on_done = std::move(on_done)](Roe<ChatSyncResult> value) {
    if (on_done) {
      on_done(std::move(value));
    }
  };
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    finish(Error("Thread not found"));
    return;
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    finish(session_epoch.error());
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    finish(sync_state.error());
    return;
  }
  const auto max_seq = chat_sync::OlderBackfillMaxSeq(*sync_state);
  if (!max_seq) {
    finish(ChatSyncResult{});
    return;
  }
  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq, std::nullopt, *max_seq,
                              kUserSyncOlderHistoryLimit, "desc");
  if (!request) {
    finish(request.error());
    return;
  }
  FetchChatTargetMessagesAsync(thread_id, *request, std::move(finish));
}

Roe<ChatSyncResult> ChatSyncWorkflow::UserInitiatedSync(const std::string& thread_id) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatSyncResult>>>();
  auto result_future = result_promise->get_future();
  UserInitiatedSyncAsync(thread_id, [result_promise](Roe<ChatSyncResult> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });
  constexpr auto kWait = std::chrono::milliseconds(36000);
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("Chat history sync timed out");
  }
  return result_future.get();
}

void ChatSyncWorkflow::UserInitiatedSyncAsync(const std::string& thread_id,
                                              std::function<void(Roe<ChatSyncResult>)> on_done) {
  auto aggregate = std::make_shared<ChatSyncResult>();
  TailSyncAsync(thread_id, [this, thread_id, aggregate, on_done = std::move(on_done)](Roe<ChatSyncResult> tail) mutable {
    if (!tail) {
      if (on_done) {
        on_done(tail.error());
      }
      return;
    }
    MergeSyncResult(*aggregate, *tail);
    RepairKnownGapAsync(thread_id, [this, thread_id, aggregate, on_done = std::move(on_done)](
                                        Roe<ChatSyncResult> gap) mutable {
      if (!gap) {
        if (on_done) {
          on_done(gap.error());
        }
        return;
      }
      MergeSyncResult(*aggregate, *gap);
      ScrollBackfillAsync(thread_id, [this, aggregate, on_done = std::move(on_done)](Roe<ChatSyncResult> older) {
        if (!older) {
          if (on_done) {
            on_done(older.error());
          }
          return;
        }
        MergeSyncResult(*aggregate, *older);
        if (on_done) {
          on_done(*aggregate);
        }
      });
    });
  });
}

Roe<ChatSyncResult> ChatSyncWorkflow::RetryGapSync(const std::string& thread_id) {
  return RepairKnownGap(thread_id);
}

void ChatSyncWorkflow::RetryGapSyncAsync(const std::string& thread_id,
                                         std::function<void(Roe<ChatSyncResult>)> on_done) {
  RepairKnownGapAsync(thread_id, std::move(on_done));
}

} // namespace pbr
