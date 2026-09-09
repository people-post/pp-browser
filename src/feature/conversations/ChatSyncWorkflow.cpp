#include "feature/conversations/ChatSyncWorkflow.h"

#include "feature/conversations/InboxController.h"
#include "feature/conversations/AttachmentFetchWorkflow.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "domain/messaging/RelayWirePayload.h"
#include "common/chat/MessagingLimits.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/ContactsStore.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include "foundation/runtime/AppRuntime.h"
#include "common/PbrCompat.h"

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
  request.requester_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  request.requester_identity_value = local_identity->relay_user_id;
  // History/stream routing stays on Brief relay ids (M010); thread peer is Account ID.
  std::string peer_relay;
  if (!thread.participant_contact_ids.empty()) {
    if (auto contact = contacts_.Get(thread.participant_contact_ids.front()); contact && *contact) {
      for (const ContactId& id : (*contact)->ids) {
        if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
          peer_relay = id.value;
          break;
        }
      }
    }
  }
  if (peer_relay.empty() && thread.peer_identity_kind == ContactIdKindToString(ContactIdKind::Account) &&
      !thread.peer_identity_value.empty()) {
    auto by_account = contacts_.FindByIdentity(thread.peer_identity_value, ContactIdKind::Account);
    if (by_account && by_account->has_value()) {
      for (const ContactId& id : (**by_account).ids) {
        if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
          peer_relay = id.value;
          break;
        }
      }
    }
  }
  if (peer_relay.empty()) {
    return Error("Direct thread missing peer relay route for history");
  }
  request.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  request.peer_identity_value = peer_relay;
  request.channel = thread.channel;
  request.session_epoch = session_epoch;
  request.limit = std::min(limit, kMaxPollBatchMessages);
  request.order = order;

  if (min_sender_seq) {
    request.min_sender_seq = std::max(*min_sender_seq, history_floor_seq + 1);
  } else if (history_floor_seq > 0) {
    request.min_sender_seq = history_floor_seq + 1;
  }
  request.max_sender_seq = max_sender_seq;
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

void ChatSyncWorkflow::CloseEmptyGap(PeerSyncState& state, const uint64_t min_seq, const uint64_t max_seq) const {
  for (uint64_t seq = min_seq; seq <= max_seq; ++seq) {
    if (std::find(state.empty_closed_seqs.begin(), state.empty_closed_seqs.end(), seq) ==
        state.empty_closed_seqs.end()) {
      state.empty_closed_seqs.push_back(seq);
    }
  }
  if (max_seq > state.contiguous_peer_seq) {
    state.contiguous_peer_seq = max_seq;
  }
  if (state.phase == PeerSyncPhase::Gap) {
    state.phase = PeerSyncPhase::Ok;
  }
}

std::optional<std::pair<uint64_t, uint64_t>> ChatSyncWorkflow::ClampGapRange(const uint64_t gap_min,
                                                                             const uint64_t gap_max,
                                                                             const uint64_t history_floor_seq) const {
  if (gap_max < gap_min) {
    return std::nullopt;
  }
  const uint64_t clamped_min = std::max(gap_min, history_floor_seq + 1);
  if (clamped_min > gap_max) {
    return std::nullopt;
  }
  if (gap_max - clamped_min + 1 > kMaxGapRepairSeqSpan) {
    return std::make_pair(clamped_min, clamped_min + kMaxGapRepairSeqSpan - 1);
  }
  return std::make_pair(clamped_min, gap_max);
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
  const bool authorized_older_backfill =
      request.max_sender_seq && sync_state->loaded_min_seq > 0 &&
      *request.max_sender_seq < sync_state->loaded_min_seq;

  auto identity = identity_.Get();
  const std::string local_account_id = identity ? (*identity).account_id : std::string{};

  for (const RelayEnvelope& envelope : response.messages) {
    const RelayReceiveOutcome outcome =
        receive_pipeline_.ProcessEnvelope(envelope, local_account_id, authorized_older_backfill, transport);
    if (outcome.persisted) {
      ++result.ingested;
      changed = true;
      if (!outcome.thread_id.empty()) {
        inbox_.OnInboundMessagePersisted(outcome.thread_id);
        if (attachment_downloads_) {
          if (auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64)) {
            if (decoded->content_type == ChatContentType::Attachment) {
              ThreadMessage message;
              message.id = envelope.message_id;
              message.thread_id = outcome.thread_id;
              message.content_type = ChatContentType::Attachment;
              message.payload_json = decoded->payload_json;
              attachment_downloads_->EnqueueFromMessage(outcome.thread_id, message);
            }
          }
        }
      }
    }
  }

  if (result.ingested == 0 && request.min_sender_seq && request.max_sender_seq) {
    auto fresh_sync_state = store_.GetPeerSyncState(thread_id, request.session_epoch);
    if (!fresh_sync_state) {
      return fresh_sync_state.error();
    }
    const std::string seq_owner = (*thread)->participant_contact_ids.empty()
                                      ? (*thread)->peer_identity_value
                                      : (*thread)->participant_contact_ids.front();
    if (PassesEmptyGapGuard(thread_id, request.session_epoch, seq_owner, *request.max_sender_seq)) {
      PeerSyncState updated = *fresh_sync_state;
      CloseEmptyGap(updated, *request.min_sender_seq, *request.max_sender_seq);
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

  const std::string seq_owner = (*thread)->participant_contact_ids.empty()
                                    ? (*thread)->peer_identity_value
                                    : (*thread)->participant_contact_ids.front();

  PeerSyncState updated = *sync_state;
  bool changed = false;
  while (updated.contiguous_peer_seq < updated.loaded_max_seq) {
    SeqRangeQuery query;
    query.session_epoch = session_epoch;
    query.seq_owner_contact_id = seq_owner;
    query.min_sender_seq = updated.contiguous_peer_seq + 1;
    query.max_sender_seq = updated.contiguous_peer_seq + 1;
    query.limit = 1;
    auto rows = store_.GetMessagesBySeqRange(thread_id, query);
    if (!rows || rows->empty()) {
      break;
    }
    updated.contiguous_peer_seq += 1;
    changed = true;
  }
  if (updated.phase == PeerSyncPhase::Gap && updated.contiguous_peer_seq == updated.loaded_max_seq) {
    updated.phase = PeerSyncPhase::Ok;
    changed = true;
  }
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

  std::optional<uint64_t> tail_min_seq;
  if (sync_state->loaded_max_seq > 0) {
    tail_min_seq = sync_state->loaded_max_seq + 1;
  }

  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq, tail_min_seq, std::nullopt,
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
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    return sync_state.error();
  }
  if (sync_state->phase == PeerSyncPhase::Compromised) {
    return Error("Sync disabled while thread is compromised");
  }

  const auto range = ClampGapRange(gap_min, gap_max, sync_state->history_floor_seq);
  if (!range) {
    return ChatSyncResult{};
  }

  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq, range->first, range->second,
                              kMaxPollBatchMessages, "asc");
  if (!request) {
    return request.error();
  }
  return FetchChatTargetMessages(thread_id, *request);
}

void ChatSyncWorkflow::MergeSyncResult(ChatSyncResult& aggregate, const ChatSyncResult& partial) const {
  aggregate.ingested += partial.ingested;
  aggregate.empty_gap_closed = aggregate.empty_gap_closed || partial.empty_gap_closed;
}

Roe<ChatSyncResult> ChatSyncWorkflow::RepairKnownGap(const std::string& thread_id) {
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    return sync_state.error();
  }
  if (sync_state->phase != PeerSyncPhase::Gap) {
    return ChatSyncResult{};
  }
  if (sync_state->loaded_max_seq <= sync_state->contiguous_peer_seq + 1) {
    return ChatSyncResult{};
  }
  return RepairGap(thread_id, sync_state->contiguous_peer_seq + 1, sync_state->loaded_max_seq - 1);
}

Roe<ChatSyncResult> ChatSyncWorkflow::ScrollBackfill(const std::string& thread_id) {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *session_epoch);
  if (!sync_state) {
    return sync_state.error();
  }
  if (sync_state->loaded_min_seq <= sync_state->history_floor_seq + 1) {
    return ChatSyncResult{};
  }

  const uint64_t max_seq = sync_state->loaded_min_seq - 1;
  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq, std::nullopt, max_seq,
                              kUserSyncOlderHistoryLimit, "desc");
  if (!request) {
    return request.error();
  }
  return FetchChatTargetMessages(thread_id, *request);
}

Roe<ChatSyncResult> ChatSyncWorkflow::UserInitiatedSync(const std::string& thread_id) {
  ChatSyncResult aggregate;

  auto tail = TailSync(thread_id);
  if (!tail) {
    return tail.error();
  }
  MergeSyncResult(aggregate, *tail);

  auto gap = RepairKnownGap(thread_id);
  if (!gap) {
    return gap.error();
  }
  MergeSyncResult(aggregate, *gap);

  auto older = ScrollBackfill(thread_id);
  if (!older) {
    return older.error();
  }
  MergeSyncResult(aggregate, *older);

  return aggregate;
}

Roe<ChatSyncResult> ChatSyncWorkflow::RetryGapSync(const std::string& thread_id) {
  return RepairKnownGap(thread_id);
}

} // namespace pbr
