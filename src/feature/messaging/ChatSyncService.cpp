#include "feature/messaging/ChatSyncService.h"

#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/people/ContactTypes.h"

#include <algorithm>

namespace pbr {

ChatSyncService::ChatSyncService(IThreadStore& store, IdentityStore& identity, IRelayClient* relay,
                                 RelayReceivePipeline& receive_pipeline)
    : store_(store), identity_(identity), relay_(relay), receive_pipeline_(receive_pipeline) {}

void ChatSyncService::SetOnMessagesChanged(std::function<void()> callback) {
  on_messages_changed_ = std::move(callback);
}

Roe<ChatHistoryRequest> ChatSyncService::BuildRequest(const Thread& thread, const uint32_t session_epoch,
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
  request.peer_identity_kind = thread.peer_identity_kind;
  request.peer_identity_value = thread.peer_identity_value;
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

bool ChatSyncService::PassesEmptyGapGuard(const std::string& thread_id, const uint32_t session_epoch,
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

void ChatSyncService::CloseEmptyGap(PeerSyncState& state, const uint64_t min_seq, const uint64_t max_seq) const {
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

std::optional<std::pair<uint64_t, uint64_t>> ChatSyncService::ClampGapRange(const uint64_t gap_min,
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

Roe<ChatSyncResult> ChatSyncService::IngestHistoryResponse(const std::string& thread_id,
                                                           const ChatHistoryRequest& request,
                                                           const ChatHistoryResponse& response) {
  if (response.session_epoch != request.session_epoch) {
    return Error("History response epoch mismatch");
  }

  ChatSyncResult result;
  bool changed = false;
  for (const RelayEnvelope& envelope : response.messages) {
    const RelayReceiveOutcome outcome = receive_pipeline_.ProcessEnvelope(envelope);
    if (outcome.persisted) {
      ++result.ingested;
      changed = true;
    }
  }

  if (changed && on_messages_changed_) {
    on_messages_changed_();
  }

  if (result.ingested == 0 && request.min_sender_seq && request.max_sender_seq) {
    auto thread = store_.GetThread(thread_id);
    if (!thread || !*thread) {
      return Error("Thread not found");
    }
    auto sync_state = store_.GetPeerSyncState(thread_id, request.session_epoch);
    if (!sync_state) {
      return sync_state.error();
    }
    const std::string seq_owner = (*thread)->participant_contact_ids.empty()
                                      ? (*thread)->peer_identity_value
                                      : (*thread)->participant_contact_ids.front();
    if (PassesEmptyGapGuard(thread_id, request.session_epoch, seq_owner, *request.max_sender_seq)) {
      PeerSyncState updated = *sync_state;
      CloseEmptyGap(updated, *request.min_sender_seq, *request.max_sender_seq);
      (void)store_.SetPeerSyncState(thread_id, request.session_epoch, updated);
      result.empty_gap_closed = true;
    }
  }

  return result;
}

Roe<ChatSyncResult> ChatSyncService::FetchChatTargetMessages(const std::string& thread_id,
                                                             ChatHistoryRequest request) {
  if (!relay_) {
    return Error("Relay client not configured");
  }

  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Direct || !ThreadChannelIsE2e((*thread)->channel)) {
    return Error("Sync requires E2E direct thread");
  }

  auto sync_state = store_.GetPeerSyncState(thread_id, request.session_epoch);
  if (!sync_state) {
    return sync_state.error();
  }
  if (sync_state->phase == PeerSyncPhase::Compromised) {
    return Error("Sync disabled while thread is compromised");
  }

  if (request.limit == 0) {
    request.limit = kDefaultTailSyncLimit;
  }
  request.limit = std::min(request.limit, kMaxPollBatchMessages);

  auto response = relay_->FetchChatHistory(request);
  if (!response) {
    return response.error();
  }
  return IngestHistoryResponse(thread_id, request, *response);
}

Roe<ChatSyncResult> ChatSyncService::TailSync(const std::string& thread_id) {
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

  auto request = BuildRequest(**thread, *session_epoch, sync_state->history_floor_seq, std::nullopt, std::nullopt,
                              kDefaultTailSyncLimit, "desc");
  if (!request) {
    return request.error();
  }
  return FetchChatTargetMessages(thread_id, *request);
}

Roe<ChatSyncResult> ChatSyncService::RepairGap(const std::string& thread_id, const uint64_t gap_min,
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

} // namespace pbr
